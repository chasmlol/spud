// Journal Menu (movie Quest_Journal): the Quests, General Stats and System tabs and the bottom bar, implemented
// and the Journal sections of (addresses there).
// The quests and misc stats come from a JournalSource (ui/journal.hpp): the host's, or data captured in the user's
// own game, or a new game's.

#include "ui/journal.hpp"
#include "ui/menus_internal.hpp"

#include "ui/player/json.hpp"

#include "ui/ui.hpp"

#include "core/log.hpp"

#include <utility>
#include <algorithm>
#include <cstdlib>
#include <format>

namespace spud::ui {

namespace {

using Value = player::Value;
namespace json = player::json;

// ---- the misc stats the engine keeps -----------------------------------------------------------------------------

// The table as the misc-stat initialiser registers it (through and): category, name
// (the game the static initialisers from on set up) and whether the stat shows while it is 0.
// Every value starts at 0. A stat that shows only while non-zero is inserted at the front of the engine's table, the
// others are appended, so the engine's order is the hidden-at-0 ones in reverse, then the rest in this order.
struct StatDef {
    int category;
    const char* name;
    bool always_shown;
};
constexpr StatDef k_stats[] = {
    {0, "Locations Discovered", true}, {0, "Dungeons Cleared", true},
    {0, "Days Passed", true}, {0, "Hours Slept", true},
    {0, "Hours Waiting", true}, {0, "Standing Stones Found", true},
    {0, "Gold Found", true}, {0, "Most Gold Carried", true},
    {0, "Chests Looted", true}, {0, "Skill Increases", true},
    {0, "Skill Books Read", true}, {0, "Food Eaten", true},
    {0, "Training Sessions", true}, {0, "Books Read", true},
    {0, "Horses Owned", true}, {0, "Houses Owned", true},
    {0, "Stores Invested In", true}, {0, "Barters", true},
    {0, "Persuasions", true}, {0, "Bribes", true},
    {0, "Intimidations", true}, {0, "Diseases Contracted", true},
    {0, "Days as a Vampire", false}, {0, "Days as a Werewolf", false},
    {0, "Necks Bitten", false}, {0, "Vampirism Cures", false},
    {0, "Werewolf Transformations", false}, {0, "Mauls", false},
    {1, "Quests Completed", true}, {1, "Misc Objectives Completed", true},
    {1, "Main Quests Completed", true}, {1, "Side Quests Completed", true},
    {1, "The Companions Quests Completed", true}, {1, "College of Winterhold Quests Completed", true},
    {1, "Thieves' Guild Quests Completed", true}, {1, "The Dark Brotherhood Quests Completed", true},
    {1, "Civil War Quests Completed", true}, {1, "Daedric Quests Completed", true},
    {1, "Dawnguard Quests Completed", false}, {1, "Dragonborn Quests Completed", false},
    {1, "Questlines Completed", true}, {2, "People Killed", true},
    {2, "Animals Killed", true}, {2, "Creatures Killed", true},
    {2, "Undead Killed", true}, {2, "Daedra Killed", true},
    {2, "Automatons Killed", true}, {2, "Favorite Weapon", true},
    {2, "Critical Strikes", true}, {2, "Sneak Attacks", true},
    {2, "Backstabs", true}, {2, "Weapons Disarmed", true},
    {2, "Brawls Won", true}, {2, "Bunnies Slaughtered", true},
    {3, "Spells Learned", true}, {3, "Favorite Spell", true},
    {3, "Favorite School", true}, {3, "Dragon Souls Collected", true},
    {3, "Words Of Power Learned", true}, {3, "Words Of Power Unlocked", true},
    {3, "Shouts Learned", true}, {3, "Shouts Unlocked", true},
    {3, "Shouts Mastered", true}, {3, "Times Shouted", true},
    {3, "Favorite Shout", true}, {4, "Soul Gems Used", true},
    {4, "Souls Trapped", true}, {4, "Magic Items Made", true},
    {4, "Weapons Improved", true}, {4, "Weapons Made", true},
    {4, "Armor Improved", true}, {4, "Armor Made", true},
    {4, "Potions Mixed", true}, {4, "Potions Used", true},
    {4, "Poisons Mixed", true}, {4, "Poisons Used", true},
    {4, "Ingredients Harvested", true}, {4, "Ingredients Eaten", true},
    {4, "Nirnroots Found", true}, {4, "Wings Plucked", true},
    {5, "Total Lifetime Bounty", true}, {5, "Largest Bounty", true},
    {5, "Locks Picked", true}, {5, "Pockets Picked", true},
    {5, "Items Pickpocketed", true}, {5, "Times Jailed", true},
    {5, "Days Jailed", true}, {5, "Fines Paid", true},
    {5, "Jail Escapes", true}, {5, "Items Stolen", true},
    {5, "Assaults", true}, {5, "Murders", true},
    {5, "Horses Stolen", true}, {5, "Trespasses", true},
    {5, "Eastmarch Bounty", false}, {5, "Falkreath Bounty", false},
    {5, "Haafingar Bounty", false}, {5, "Hjaalmarch Bounty", false},
    {5, "The Pale Bounty", false}, {5, "The Reach Bounty", false},
    {5, "The Rift Bounty", false}, {5, "Tribal Orcs Bounty", false},
    {5, "Whiterun Bounty", false}, {5, "Winterhold Bounty", false},
    {6, "NumVampirePerks", true}, {6, "NumWerewolfPerks", true},
    {6, "Solstheim Locations Discovered", true}, {6, "StalhrimItemsCrafted", true},
};

// The four stats whose value is a name: the most used weapon, spell, school and shout, or "None" (the
// string) while there is none.
bool is_favorite(std::string_view name) {
    return name == "Favorite Weapon" || name == "Favorite Spell" || name == "Favorite School" ||
           name == "Favorite Shout";
}

} // namespace

std::vector<MiscStat> journal_stat_rows() {
    std::vector<MiscStat> out;
    for (const StatDef& d : k_stats) {
        MiscStat m;
        m.name = d.name;
        m.category = d.category;
        m.always_shown = d.always_shown;
        if (is_favorite(m.name)) {
            m.value = std::string("None");
        } else {
            m.value = 0.0;
        }
        if (d.always_shown) {
            out.push_back(std::move(m));
        } else {
            out.insert(out.begin(), std::move(m));
        }
    }
    return out;
}

namespace {

const StatDef* find_stat(std::string_view name) {
    for (const StatDef& d : k_stats) {
        if (name == d.name) {
            return &d;
        }
    }
    return nullptr;
}

// ---- the Journal's data from a capture, or a new game's ----------------------------------------------------------

// The movie paths the game captures in the game (capture-journal.osc): the title list after the engine filled it
// and the movie sorted it, with each quest's objectives and description (requested by selecting it); whether Show
// on Map is allowed; and the stats categories after PopulateStatsList.
constexpr std::string_view k_title_entries = "_root.QuestJournalFader.Menu_mc.QuestsFader.Page_mc.TitleList.entryList";
constexpr std::string_view k_show_on_map = "_root.QuestJournalFader.Menu_mc.QuestsFader.Page_mc.bAllowShowOnMap";
constexpr std::string_view k_stat_categories =
    "_root.QuestJournalFader.Menu_mc.StatsFader.Page_mc.CategoryList.entryList";

const json::Value* member(const json::Value& o, std::string_view name) {
    for (const auto& [k, v] : o.members) {
        if (k == name) {
            return &v;
        }
    }
    return nullptr;
}

double num(const json::Value& o, std::string_view name, double fallback = 0) {
    const json::Value* v = member(o, name);
    if (!v) {
        return fallback;
    }
    return v->kind == json::Value::number ? v->n : v->kind == json::Value::boolean ? (v->b ? 1.0 : 0.0) : fallback;
}

bool flag(const json::Value& o, std::string_view name) { return num(o, name) != 0; }

std::string text(const json::Value& o, std::string_view name) {
    const json::Value* v = member(o, name);
    return v && v->kind == json::Value::string ? v->s : std::string();
}

class CapturedJournal final : public JournalSource {
public:
    // `paths`: the capture's "paths" object, or null for a new game.
    explicit CapturedJournal(const json::Value* paths) {
        if (paths) {
            read(*paths);
        }
        if (!m_have_stats) {
            new_game_stats();
        }
    }

    std::vector<JournalQuest> quests() override {
        std::vector<JournalQuest> out;
        for (const Quest& q : m_quests) {
            out.push_back(q.entry);
        }
        return out;
    }
    bool has_misc_objectives() override { return m_misc.has_value(); }
    std::optional<JournalObjectives> objectives(uint32_t form_id, uint32_t instance) override {
        if (form_id == 0 && instance == 0) {
            return m_misc ? *m_misc : JournalObjectives{};
        }
        for (const Quest& q : m_quests) {
            if (q.entry.form_id == form_id && q.entry.instance == instance) {
                return q.objectives;
            }
        }
        for (const Quest& q : m_quests) {
            if (q.entry.form_id == form_id) {
                return JournalObjectives{}; // another instance of a quest the capture knows: none listed
            }
        }
        return std::nullopt;
    }
    std::optional<bool> toggle_active(uint32_t form_id, uint32_t instance) override {
        for (Quest& q : m_quests) {
            if (q.entry.form_id == form_id && q.entry.instance == instance) {
                q.entry.active = !q.entry.active;
                return q.entry.active;
            }
        }
        // A misc quest, toggled from its objective under MISCELLANEOUS: every objective of it shows the state.
        std::optional<bool> now;
        if (m_misc) {
            for (JournalObjective& o : m_misc->objectives) {
                if (o.form_id == form_id && o.instance == instance) {
                    if (!now) {
                        now = !o.active;
                    }
                    o.active = *now;
                }
            }
        }
        return now;
    }
    bool quest_completed(uint32_t form_id) override {
        bool any = false;
        for (const Quest& q : m_quests) {
            if (q.entry.form_id == form_id) {
                if (!q.entry.completed) {
                    return false;
                }
                any = true;
            }
        }
        return any;
    }
    std::vector<MiscStat> misc_stats() override { return m_stats; }

    std::optional<bool> misc_active() const { return m_misc_active; }
    std::optional<bool> show_on_map() const { return m_show_on_map; }
    bool from_capture() const { return m_from_capture; }

private:
    struct Quest {
        JournalQuest entry;
        JournalObjectives objectives;
        double order = 0; // the movie's timeIndex: the position in the engine's list
    };

    static JournalObjectives read_objectives(const json::Value& entry) {
        JournalObjectives o;
        if (const json::Value* list = member(entry, "objectives"); list && list->kind == json::Value::array) {
            for (const json::Value& item : list->items) {
                JournalObjective j;
                j.text = text(item, "text");
                j.completed = flag(item, "completed");
                j.failed = flag(item, "failed");
                if (const json::Value* t = member(item, "questTargetID"); t && t->kind == json::Value::number) {
                    j.target = t->n;
                }
                j.active = flag(item, "active");
                j.form_id = uint32_t(num(item, "formID"));
                j.instance = uint32_t(num(item, "instance"));
                o.objectives.push_back(std::move(j));
            }
        }
        if (const json::Value* d = member(entry, "description"); d && d->kind == json::Value::string) {
            o.description = d->s;
        }
        return o;
    }

    void read(const json::Value& paths) {
        for (const auto& [path, v] : paths.members) {
            if (path == k_title_entries && v.kind == json::Value::array) {
                m_from_capture = true;
                for (const json::Value& e : v.items) {
                    if (e.kind != json::Value::object || flag(e, "divider")) {
                        continue; // the divider the movie puts between open and completed quests
                    }
                    const uint32_t form_id = uint32_t(num(e, "formID")), instance = uint32_t(num(e, "instance"));
                    if (form_id == 0 && instance == 0) {
                        // MISCELLANEOUS, added by the movie when the engine says there are misc objectives.
                        m_misc = read_objectives(e);
                        m_misc_active = flag(e, "active");
                        continue;
                    }
                    Quest q;
                    q.entry = {text(e, "text"), form_id, instance, flag(e, "active"), flag(e, "completed"),
                               int(num(e, "type"))};
                    q.objectives = read_objectives(e);
                    q.order = num(e, "timeIndex");
                    m_quests.push_back(std::move(q));
                }
                std::stable_sort(m_quests.begin(), m_quests.end(),
                                 [](const Quest& a, const Quest& b) { return a.order < b.order; });
            } else if (path == k_show_on_map && v.kind == json::Value::boolean) {
                m_show_on_map = v.b;
            } else if (path == k_stat_categories && v.kind == json::Value::array) {
                m_from_capture = true;
                m_have_stats = true;
                for (size_t c = 0; c < v.items.size(); ++c) {
                    const json::Value* stats = member(v.items[c], "stats");
                    if (!stats || stats->kind != json::Value::array) {
                        continue;
                    }
                    for (const json::Value& s : stats->items) {
                        MiscStat m;
                        m.name = text(s, "text");
                        if (m.name.starts_with('$')) {
                            m.name.erase(0, 1); // the movie shows "$" + name
                        }
                        m.category = int(c);
                        const json::Value* value = member(s, "value");
                        if (value && value->kind == json::Value::string) {
                            m.value = value->s;
                        } else {
                            m.value = value && value->kind == json::Value::number ? value->n : 0.0;
                        }
                        const StatDef* d = find_stat(m.name);
                        m.always_shown = d ? d->always_shown : true;
                        m_stats.push_back(std::move(m));
                    }
                }
            }
        }
    }

    void new_game_stats() {
        // Every stat at 0, each favourite "None": what shows are the always-shown stats in registration order.
        for (const StatDef& d : k_stats) {
            if (!d.always_shown) {
                continue;
            }
            MiscStat m;
            m.name = d.name;
            m.category = d.category;
            m.always_shown = true;
            if (is_favorite(m.name)) {
                m.value = std::string("None");
            } else {
                m.value = 0.0;
            }
            m_stats.push_back(std::move(m));
        }
    }

    std::vector<Quest> m_quests;
    std::optional<JournalObjectives> m_misc;
    std::optional<bool> m_misc_active, m_show_on_map;
    std::vector<MiscStat> m_stats;
    bool m_have_stats = false;
    bool m_from_capture = false;
};

// What the engine keeps in globals across openings of the Journal: the quest selected when it last closed
// (, both -1 in the image) and whether misc objectives are tracked (saved with the
// game). Here also the captured data with the tracking changes made since.
struct JournalGlobals {
    int saved_quest = -1;
    int saved_instance = -1;
    std::optional<bool> show_misc;
    std::unique_ptr<CapturedJournal> captured;
};

JournalGlobals& globals() {
    static JournalGlobals g;
    return g;
}

class JournalMenu final : public Menu {
public:
    explicit JournalMenu(Ui& ui) : Menu(ui, {"Journal Menu", "Quest_Journal", ScaleMode::exact_fit, 5, 0xf29, 0, 12}) {}

private:
    // The constructor puts the HUD into "JournalMode"; the destructor ends it
    HudModeHold m_hud_mode{m_ui, "JournalMode"};
    BlurHold m_blur{m_ui};
    bool m_graphics_added = false, m_graphics_open = false, m_graphics_stale = false;

public:

    void on_show() override {
        // JournalMenu::ProcessMessage, show: RestoreSavedSettings(the remembered tab when the show
        // message's byte is set, else 0; tabs disabled), then UIJournalOpen.
        const bool remembered = m_ui.take_journal_remembered();
        send("RestoreSavedSettings", {Value::number(remembered ? m_ui.journal_tab() : 0), Value::boolean(false)});
        m_ui.host().ui_play_sound("UIJournalOpen");
    }
    bool on_hide() override {
        if (m_closing_done) {
            return true;
        }
        send("StartCloseMenu", {Value::boolean(true)});
        return false;
    }
    void advance(double dt, double engine_ms) override {
        add_graphics_page();
        if (std::exchange(m_graphics_stale, false)) {
            fill_graphics(); // not inside the movie's own input handling
        }
        // The "Journal Setttings Saved" update message (sic): queued, so never inside the movie's own call. The
        // System page stays on "Saving..." and ignores input until it comes.
        if (m_settings_saved) {
            m_settings_saved = false;
            send("SettingsSaved");
        }
        Menu::advance(dt, engine_ms);
    }

private:
    using Object = player::Object;
    Value m_mapping_list; // the list a remap refills

    JournalSource& source() {
        if (JournalSource* s = m_ui.host().ui_journal()) {
            return *s;
        }
        JournalGlobals& g = globals();
        if (!g.captured) {
            const json::Value* c = m_ui.capture("journal");
            g.captured = std::make_unique<CapturedJournal>(c ? member(*c, "paths") : nullptr);
            if (!g.captured->from_capture()) {
                log::info("ui Journal Menu: no capture 'journal'; the Quests and General Stats tabs show a new game");
            }
        }
        return *g.captured;
    }
    CapturedJournal* captured() { return m_ui.host().ui_journal() ? nullptr : globals().captured.get(); }

    void register_handlers() override {
        handle("CloseMenu", [this](Call& c) {
            // CloseMenu(tab, selectedQuestID, selectedQuestInstance) keeps the selection when both are
            // numbers, then hides the menu.
            if (c.arg(1).is_number() && c.arg(2).is_number()) {
                globals().saved_quest = int(c.arg(1).n);
                globals().saved_instance = int(c.arg(2).n);
            }
            m_closing_done = true;
            m_ui.close_now(name());
        });
        handle("RememberCurrentTabIndex", [this](Call& c) { m_ui.set_journal_tab(int(player()->vm.to_number(c.arg(0)))); });
        handle("PlaySound", [this](Call& c) {
            if (c.args.size() == 1) {
                m_ui.host().ui_play_sound(*player()->vm.to_string(c.arg(0)));
            }
        });

        // ---- Quests tab (accept) --------------------------------------------
        handle("RequestQuestsData", [this](Call& c) { request_quests(c); });
        handle("RequestObjectivesData", [this](Call&) { request_objectives(); });
        handle("ToggleQuestActiveStatus", [this](Call& c) {
            // (formID, instance) of the current instance of a quest: toggles its tracking and answers with
            // the new state; no answer when there is no such quest instance.
            auto& vm = player()->vm;
            const auto now = source().toggle_active(uint32_t(vm.to_number(c.arg(0))), uint32_t(vm.to_number(c.arg(1))));
            if (now) {
                c.respond({Value::boolean(*now)});
            }
        });
        handle("ToggleShowMiscObjectives", [this](Call& c) {
            // the flag, then UIQuestActive or UIQuestInactive.
            const bool on = player()->vm.to_boolean(c.arg(0));
            globals().show_misc = on;
            m_ui.host().ui_play_sound(on ? "UIQuestActive" : "UIQuestInactive");
        });
        handle("ShowTargetOnMap", [this](Call& c) {
            // a show message to MapMenu (an update when it is open) with the target's reference handle,
            // then the Tween Menu closes if it is open. The movie has closed the Journal just before.
            log::info("ui Journal Menu: quest target {:08X} on the map", uint32_t(player()->vm.to_number(c.arg(0))));
            m_ui.open("MapMenu");
            if (m_ui.is_open("TweenMenu")) {
                m_ui.close("TweenMenu");
            }
        });

        // ---- General Stats tab (accept) --------------------------------------
        handle("updateStats", [this](Call& c) {
            // with GetMiscStatInfoFunc: for every stat outside category 6 that shows (always,
            // or while not 0), its name, value, category and always-shown flag.
            std::vector<Value> values;
            for (const MiscStat& s : source().misc_stats()) {
                const bool zero = std::holds_alternative<double>(s.value) && std::get<double>(s.value) == 0;
                if (s.category == 6 || (!s.always_shown && zero)) {
                    continue;
                }
                values.push_back(Value::string(s.name));
                values.push_back(std::holds_alternative<double>(s.value)
                                     ? Value::number(std::get<double>(s.value))
                                     : Value::string(std::get<std::string>(s.value)));
                values.push_back(Value::number(s.category));
                values.push_back(Value::boolean(s.always_shown));
            }
            c.respond(std::move(values));
        });

        // ---- System tab (controls, settings) ---------------
        handle("RequestIsOnPC", [](Call& c) { c.respond({Value::boolean(true)}); });
        handle("QuitToMainMenu", [this](Call&) { m_ui.host().ui_action("main_menu"); });
        handle("QuitToDesktop", [this](Call&) { m_ui.host().ui_action("quit"); });
        handle("SetVersionText", [this](Call& c) {
            // "%s.%d" of the version and the GMST iUpdateESMVersion (compiled default 4; Update.esm sets 8).
            const Value tf = c.arg(0);
            if (tf.is_object()) {
                const std::string v = m_ui.game_setting("iUpdateESMVersion");
                const int esm = v.empty() ? 4 : std::atoi(v.c_str());
                player()->vm.call_method(tf, "SetText", {Value::string(std::format("1.7.104.0.{}", esm))});
            }
        });
        handle("RequestPlayerInfo", [this](Call& c) {
            // the date text, the level, the progress towards the next level in percent (SetPlayerInfo).
            const PlayerInfo p = m_ui.host().ui_player_info();
            c.respond({Value::string(m_ui.date_text(m_ui.calendar(p))), Value::number(p.level),
                       Value::number(p.progress_percent)});
        });
        // answers with the byte, 1 in the image and written nowhere: the movie's
        // SetShowMod(true) lists the System tab's mod entry, as the game's list shows.
        handle("ShouldShowMod", [](Call& c) { c.respond({Value::boolean(true)}); });
        // with the category list's entries: QUICKSAVE and SAVE by CanSaveGame, LOAD by CanLoadGame and there
        // being saves.
        handle("SetSaveDisabled", [this](Call& c) {
            SaveSource* saves = m_ui.host().ui_saves();
            auto& vm = player()->vm;
            const bool no_save = !saves || !saves->can_save(), no_load = !saves || !saves->can_load();
            for (size_t i = 0; i < 3 && i < c.args.size(); ++i) {
                if (c.arg(i).is_object()) {
                    vm.set(c.arg(i), "disabled", Value::boolean(i < 2 ? no_save : no_load));
                }
            }
        });
        for (const char* noop : {"myLog", "PopulateHelpTopics",
                                 "PopulateCreationClubTopics", "RequestHelpText", "RequestCreationClubText",
                                 "GetButtonFromUserEvent", "ShouldShowKinectTunerOption", "OpenKinectTuner",
                                 "ModManager"}) {
            handle(noop, [](Call&) {});
        }
        handle("SaveSettings", [this](Call&) { save_settings(); });
        handle("SaveControls", [this](Call&) { save_settings(); });
        // Settings pages (section 4, Journal Menu: System tab): the engine writes ID, value and defaultVal into the
        // movie's entries and applies OptionChange.
        for (const char* page : {"RequestGameplayOptions", "RequestDisplayOptions", "RequestAudioOptions"}) {
            handle(page, [this, page = std::string(page)](Call& c) { fill_options(page, c.arg(0)); });
        }
        handle("OptionChange", [this](Call& c) {
            const int id = int(player()->vm.to_number(c.arg(0)));
            m_ui.host().ui_option_change(id, player()->vm.to_number(c.arg(1)));
            m_graphics_stale = id >= k_graphics_first_id && m_graphics_open; // a preset moves the other rows, a row the preset
        });
        handle("RequestInputMappings", [this](Call& c) { fill_input_mappings(c.arg(0)); });
        handle("ResetControlsToDefaults", [this](Call&) { m_ui.reset_controls(); });
        // the next press of the device in use decides; FinishRemapMode says how it went.
        handle("StartRemapMode", [this](Call& c) {
            auto& vm = player()->vm;
            m_mapping_list = c.arg(1);
            m_ui.start_remap(*vm.to_string(c.arg(0)), [this](bool took) {
                if (took && m_mapping_list.is_object()) {
                    m_mapping_list.o->elements.clear();
                    fill_input_mappings(m_mapping_list);
                }
                send("FinishRemapMode", {Value::boolean(took)});
            });
        });
        handle_save_load();
        // The save panel also calls UseCurrentCharacterFilter, which the game never registers (the name is not
        // among its strings): the delegate drops it, as here.
        handle("UseCurrentCharacterFilter", [](Call&) {});
    }

    // for each binding of the keyboard and mouse.
    void fill_input_mappings(const Value& list) {
        if (!list.is_object()) {
            return;
        }
        auto& vm = player()->vm;
        for (const Ui::InputMapping& m : m_ui.input_mappings()) {
            Object* entry = vm.make_object();
            const Value v = Value::object(entry);
            vm.set(v, "text", Value::string(m.event));
            vm.set(v, "buttonName", Value::string(m.button));
            vm.set(v, "sortIndex", Value::number(m.sort_index));
            vm.set(v, "buttonID", Value::number(double(m.code)));
            list.o->elements.push_back(v);
        }
    }

    void request_quests(Call& c) {
        // keeps the list (RequestObjectivesData reads its selectedEntry), pushes one entry per quest onto
        // its entryList, forgets the remembered selection when that quest is completed, and answers with the
        // remembered quest and instance, whether there are misc objectives, whether they are tracked, and whether
        // Show on Map is allowed.
        auto& vm = player()->vm;
        m_list = c.arg(0);
        JournalSource& src = source();
        const Value entries = m_list.is_object() ? vm.get(m_list, "entryList") : Value{};
        if (entries.is_object()) {
            for (const JournalQuest& q : src.quests()) {
                const Value e = Value::object(vm.make_object());
                vm.set(e, "text", Value::string(q.text));
                vm.set(e, "formID", Value::number(q.form_id));
                vm.set(e, "instance", Value::number(q.instance));
                vm.set(e, "active", Value::boolean(q.active));
                vm.set(e, "completed", Value::number(q.completed ? 1 : 0));
                vm.set(e, "type", Value::number(q.type));
                entries.o->elements.push_back(e);
            }
        }
        JournalGlobals& g = globals();
        if (g.saved_quest != -1 && src.quest_completed(uint32_t(g.saved_quest))) {
            g.saved_quest = g.saved_instance = -1;
        }
        CapturedJournal* cap = captured();
        if (!g.show_misc) {
            g.show_misc = cap ? cap->misc_active().value_or(false) : false; // .bss: 0 until a game sets it
        }
        // Show on Map is allowed unless the UI's counter or the byte the game is set, as nothing
        // in spud does; a capture holds the game's answer.
        const bool show_on_map = cap ? cap->show_on_map().value_or(true) : true;
        c.respond({Value::number(g.saved_quest), Value::number(g.saved_instance),
                   Value::boolean(src.has_misc_objectives()), Value::boolean(*g.show_misc),
                   Value::boolean(show_on_map)});
    }

    void request_objectives() {
        // the kept list's selectedEntry (formID and instance, both 0 for MISCELLANEOUS) gets "objectives",
        // a new array of {text, completed, failed, [active, formID, instance under MISCELLANEOUS], questTargetID},
        // and a quest also its "description". Nothing when the entry is no quest.
        if (!m_list.is_object()) {
            return;
        }
        auto& vm = player()->vm;
        const Value entry = vm.get(m_list, "selectedEntry");
        if (!entry.is_object()) {
            return;
        }
        const uint32_t form_id = uint32_t(vm.to_number(vm.get(entry, "formID")));
        const uint32_t instance = uint32_t(vm.to_number(vm.get(entry, "instance")));
        const bool misc = form_id == 0 && instance == 0;
        const std::optional<JournalObjectives> o = source().objectives(form_id, instance);
        if (!o) {
            return;
        }
        std::vector<Value> items;
        for (const JournalObjective& j : o->objectives) {
            const Value e = Value::object(vm.make_object());
            vm.set(e, "text", Value::string(j.text));
            vm.set(e, "completed", Value::number(j.completed ? 1 : 0));
            vm.set(e, "failed", Value::number(j.failed ? 1 : 0));
            if (misc) {
                vm.set(e, "active", Value::boolean(j.active));
                vm.set(e, "formID", Value::number(j.form_id));
                vm.set(e, "instance", Value::number(j.instance));
            }
            if (j.target) {
                vm.set(e, "questTargetID", Value::number(*j.target));
            }
            items.push_back(e);
        }
        vm.set(entry, "objectives", Value::object(vm.make_array(std::move(items))));
        if (!misc) {
            vm.set(entry, "description", Value::string(o->description.value_or("")));
        }
    }

    // The Graphics page, beside the movie's three (docs/UI.md): a fourth entry in its Settings list, and its press handler
    // wrapped so the option list holds our rows when the movie's own code, which knows no fourth page, goes on to show it.
    static constexpr int k_graphics_first_id = 100;
    static constexpr double k_graphics_index = 3;

    Value system_page() {
        auto& vm = player()->vm;
        Value at = vm.get(Value::object(player()->root_object()), "QuestJournalFader");
        for (const char* name : {"Menu_mc", "SystemFader", "Page_mc"}) {
            if (!at.is_object()) {
                return {};
            }
            at = vm.get(at, name);
        }
        return at;
    }

    void add_graphics_page() {
        if (m_graphics_added) {
            return;
        }
        auto& vm = player()->vm;
        const Value page = system_page();
        const Value list = page.is_object() ? vm.get(page, "SettingsList") : Value{};
        const Value entries = list.is_object() ? vm.call_method(list, "__get__entryList", {}) : Value{};
        if (!entries.is_object() || entries.o->elements.size() != 3) {
            return; // the page isn't up yet
        }
        m_graphics_added = true;
        const Value entry = Value::object(vm.make_object());
        vm.set(entry, "text", Value::string("Graphics"));
        entries.o->elements.push_back(entry);
        // The list has a clip for each of its three entries: a fourth below them, as its onLoad sets one up.
        const Value last = vm.get(list, "Entry2");
        vm.call_method(last, "duplicateMovieClip", {Value::string("Entry3"), Value::number(16000)});
        const Value clip = vm.get(list, "Entry3");
        vm.set(clip, "clipIndex", Value::number(k_graphics_index));
        for (const char* handler : {"onRollOver", "onPress", "onPressAux"}) {
            vm.set(clip, handler, vm.get(last, handler));
        }
        vm.set(list, "iMaxItemsShown", Value::number(k_graphics_index + 1));
        const Value border = vm.get(list, "border"); // the list's height, sized for three
        vm.set(border, "_height", Value::number(2 * vm.to_number(vm.get(border, "_height"))));
        vm.set(list, "fListHeight", vm.get(border, "_height"));
        vm.call_method(list, "InvalidateData", {});
        const Value original = vm.get(page, "onSettingsCategoryPress");
        vm.set(page, "onSettingsCategoryPress", Value::object(vm.make_function([this, original, page, list](ui::player::NativeCall& c) {
            m_graphics_open = c.vm.to_number(c.vm.call_method(list, "__get__selectedIndex", {})) == k_graphics_index;
            if (m_graphics_open) {
                fill_graphics();
            }
            return c.vm.call(original, page, c.args);
        })));
    }

    void fill_graphics() {
        auto& vm = player()->vm;
        const Value page = system_page();
        Value list = page.is_object() ? vm.get(page, "OptionsListsPanel") : Value{};
        for (const char* name : {"OptionsLists", "List_mc"}) {
            list = list.is_object() ? vm.get(list, name) : Value{};
        }
        if (!list.is_object()) {
            return;
        }
        std::vector<Value> rows;
        for (const GraphicsOption& o : m_ui.host().ui_graphics_options()) {
            const Value row = Value::object(vm.make_object());
            vm.set(row, "text", Value::string(o.text));
            vm.set(row, "movieType", Value::number(o.options.empty() ? 2 : 1));
            if (!o.options.empty()) {
                std::vector<Value> steps;
                for (const std::string& s : o.options) {
                    steps.push_back(Value::string(s));
                }
                vm.set(row, "options", Value::object(vm.make_array(std::move(steps))));
            }
            vm.set(row, "ID", Value::number(o.id));
            vm.set(row, "value", Value::number(o.value));
            vm.set(row, "defaultVal", Value::number(o.default_value));
            rows.push_back(row);
        }
        vm.call_method(list, "__set__entryList", {Value::object(vm.make_array(std::move(rows)))});
        const Value overwrite = vm.get(list, "bAllowValueOverwrite"); // rows keep their values without it
        vm.set(list, "bAllowValueOverwrite", Value::boolean(true));
        vm.call_method(list, "InvalidateData", {});
        vm.set(list, "bAllowValueOverwrite", overwrite);
    }

    void fill_options(const std::string& page, const Value& entries) {
        if (!entries.is_object()) {
            return;
        }
        auto& vm = player()->vm;
        const std::vector<OptionValue> values = m_ui.host().ui_options(page);
        Object* list = entries.o;
        for (size_t i = 0; i < values.size(); ++i) {
            const OptionValue& o = values[i];
            Value entry;
            if (i < list->elements.size()) {
                entry = list->elements[i];
            }
            if (o.appended || !entry.is_object()) {
                // Sound categories are pushed as new entries {text, ID, value, defaultVal}.
                Object* e = vm.make_object();
                entry = Value::object(e);
                vm.set(entry, "text", Value::string(o.text));
                if (o.checkbox) {
                    // Read-only: when the request returns, the Audio page makes every entry a slider.
                    vm.define(e, "movieType", Value::number(2), ui::player::prop_read_only);
                }
                list->elements.push_back(entry);
            }
            if (!o.written) {
                continue; // entries the PC build leaves as the movie made them
            }
            vm.set(entry, "ID", Value::number(o.id));
            vm.set(entry, "value", Value::number(o.value));
            vm.set(entry, "defaultVal", Value::number(o.default_value));
        }
    }

    void save_settings() {
        m_ui.host().ui_settings_closed();
        m_settings_saved = true;
    }

    Value m_list; // the title list RequestQuestsData was given (a clip of this movie)
    bool m_closing_done = false;
    bool m_settings_saved = false; // saved: the movie is told on the next update
};

} // namespace

std::unique_ptr<Menu> create_journal_menu(Ui& ui, const std::string& name) {
    if (name == "Journal Menu") {
        return std::make_unique<JournalMenu>(ui);
    }
    return nullptr;
}

} // namespace spud::ui
