// Magic, Favorites and Tween menus, implemented (addresses and evidence there).
//
// The engine side of these menus is mostly data from game systems spud does not have yet: the player's
// spells, powers, shouts and active effects, their item cards, the bottom bar's player stats, the favourites.
// That data sits behind MagicSource; today CapturedMagic replays what the game's own engine wrote into the
// movies, captured by the game. The menus' contract (what
// the engine calls in the movie, in which order, and what each delegate call does) is the game's and does not
// depend on where the data comes from.

#include "ui/item3d.hpp"
#include "ui/items.hpp"
#include "ui/menus_internal.hpp"

#include "ui/player/json.hpp"
#include "ui/ui.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace spud::ui {

namespace {

using Value = player::Value;
using Json = player::json::Value;

const Json* member(const Json* o, std::string_view name) {
    if (!o || o->kind != Json::object) {
        return nullptr;
    }
    for (const auto& [k, v] : o->members) {
        if (k == name) {
            return &v;
        }
    }
    return nullptr;
}

std::string text_of(const Json* o, std::string_view name) {
    const Json* v = member(o, name);
    return v && v->kind == Json::string ? v->s : std::string();
}

double number_of(const Json* o, std::string_view name, double fallback = 0) {
    const Json* v = member(o, name);
    return v && v->kind == Json::number ? v->n : fallback;
}

void set_member(Json& o, const std::string& name, Json v) {
    for (auto& [k, m] : o.members) {
        if (k == name) {
            m = std::move(v);
            return;
        }
    }
    o.members.emplace_back(name, std::move(v));
}

Json number_json(double n) {
    Json j;
    j.kind = Json::number;
    j.n = n;
    return j;
}

// One variable of a the game capture, or null.
const Json* captured_path(Ui& ui, const std::string& capture, std::string_view path) {
    return member(member(ui.capture(capture), "paths"), path);
}

// An engine-made object from a capture as a value of `vm`. the game writes `undefined` as null, and these
// engine objects never hold null (their members are numbers, strings, booleans, or copies of members an entry
// may lack), so null goes back to undefined. With `only`, members the movie added (clipIndex, ...) are left
// out and the rest come in the order the engine sets them.
Value engine_value(player::Vm& vm, const Json& j, std::initializer_list<std::string_view> only = {}) {
    if (j.kind == Json::null) {
        return Value{};
    }
    if (j.kind != Json::object) {
        return player::json_to_value(vm, j);
    }
    Value o = Value::object(vm.make_object());
    if (only.size() == 0) {
        for (const auto& [k, m] : j.members) {
            vm.set(o, k, engine_value(vm, m));
        }
        return o;
    }
    for (std::string_view k : only) {
        if (const Json* m = member(&j, k)) {
            vm.set(o, k, engine_value(vm, *m));
        }
    }
    return o;
}

// ---- the data the engine supplies ----------------------------------------------------------------------

// What the Magic and Favorites menus show, from the game systems that own it. Entries are the objects the
// engine builds for the movies (MagicItemData,; the Favorites entries), as JSON.
class MagicSource {
public:
    virtual ~MagicSource() = default;
    // The Magic Menu's entries in the engine's order (sorted by text, hidden ones included): text, count,
    // equipState, filterFlag, favorite, enabled, and for active effects cardName, strEffects, timeRemaining,
    // negativeEffect.
    virtual const std::vector<Json>& spells() = 0;
    // RequestItemCardInfo's answer for an entry (ItemCard plus the entry's name, effects,
    // negativeEffect and timeRemaining), or null when there is none.
    virtual const Json* item_card(const Json& entry) = 0;
    // UpdatePlayerInfo's object (BottomBar: damage, armor, gold, health, magicka,...).
    virtual const Json* player_info() = 0;
    // The Favorites Menu's entries: text, index, equipState, hotkey.
    virtual std::vector<Json> favorites() = 0;
    // MagicFavorites ( add, remove): YButton in the Magic Menu.
    virtual void toggle_favorite(const std::string& text) = 0;
    // ActorEquipManager ( spell, shout, object) into a hand (0 any, 1 right, 2
    // left). False when there is no equipment system behind the data.
    virtual bool equip(const std::string& text, int slot) = 0;
    // A word of this shout the player knows but has not unlocked (XButton spends a dragon soul on it).
    virtual bool has_word_to_unlock(const std::string& text) = 0;
    virtual double dragon_souls() = 0; // the DragonSouls actor value (133)
    virtual bool shouting() = 0;       // on the player
    // Why the player can't cast this spell now (the cast check's reason through); empty if unknown.
    virtual std::string cast_failure(const std::string& text) = 0;
    // The model the 3D item shows for an entry (its menu display object, MDOB, as the game
    // picks it for spells, shouts and effects), as a cache.osm path; "" for none.
    virtual std::string model(const Json& entry) = 0;
};

// The data as the game's engine wrote it, captured by the game: "magicmenu" (the entries and the player
// info), "magicmenu-card-<n>" (one item card each) and "favoritesmenu".
class CapturedMagic final : public MagicSource {
public:
    explicit CapturedMagic(Ui& ui) : m_ui(ui) {}

    const std::vector<Json>& spells() override {
        load();
        return m_spells;
    }
    const Json* item_card(const Json& entry) override {
        load();
        // The card's name is the entry's cardName for an active effect, otherwise its text.
        const std::string name =
            member(&entry, "cardName") ? text_of(&entry, "cardName") : text_of(&entry, "text");
        const bool active = (int(number_of(&entry, "filterFlag")) & k_active_effects) != 0;
        for (const auto& c : m_cards) {
            if (text_of(&c.card, "name") == name && c.active == active) {
                return &c.card;
            }
        }
        return nullptr;
    }
    const Json* player_info() override {
        load();
        return m_player.kind == Json::object ? &m_player : nullptr;
    }
    std::vector<Json> favorites() override {
        load();
        // The favourites store keeps the order things were favourited in; spells favourited or unfavourited
        // since the capture follow the Magic Menu's entries.
        std::vector<Json> out;
        for (const Json& f : m_favorites) {
            const std::string t = text_of(&f, "text");
            const Json* spell = find_spell(t);
            if (spell && number_of(spell, "favorite") == 0) {
                continue;
            }
            Json e = f;
            if (spell) {
                set_member(e, "equipState", number_json(number_of(spell, "equipState")));
            }
            out.push_back(std::move(e));
        }
        for (const Json& s : m_spells) {
            const std::string t = text_of(&s, "text");
            const bool listed =
                std::any_of(out.begin(), out.end(), [&](const Json& f) { return text_of(&f, "text") == t; });
            if (number_of(&s, "favorite") != 0 && !listed &&
                (int(number_of(&s, "filterFlag")) & k_active_effects) == 0) {
                Json e;
                e.kind = Json::object;
                set_member(e, "text", *member(&s, "text"));
                set_member(e, "index", number_json(0));
                set_member(e, "equipState", number_json(number_of(&s, "equipState")));
                set_member(e, "hotkey", number_json(-1));
                out.push_back(std::move(e));
            }
        }
        for (size_t i = 0; i < out.size(); ++i) {
            set_member(out[i], "index", number_json(double(i))); // the entry's place in the engine's array
        }
        return out;
    }
    void toggle_favorite(const std::string& text) override {
        load();
        // Measured (capture-magic): a favourite has favorite 1 and bit 1 (the FAVORITES category) in
        // filterFlag.
        for (Json& s : m_spells) {
            if (text_of(&s, "text") == text && (int(number_of(&s, "filterFlag")) & k_active_effects) == 0) {
                const bool fav = number_of(&s, "favorite") == 0;
                set_member(s, "favorite", number_json(fav ? 1 : 0));
                const int flag = int(number_of(&s, "filterFlag"));
                set_member(s, "filterFlag", number_json(double(fav ? flag | 1 : flag & ~1)));
                return;
            }
        }
    }
    bool equip(const std::string&, int) override {
        if (!m_equip_logged) {
            m_equip_logged = true;
            log::info("ui: equipping a spell or shout needs the player's equipment (ActorEquipManager); the "
                      "captured magic data stays as captured");
        }
        return false;
    }
    bool has_word_to_unlock(const std::string&) override { return false; } // the capture holds no word states
    double dragon_souls() override {
        const Json* p = player_info();
        // "0 Dragon Souls": the bottom bar's text is the only trace of the count in the capture.
        const std::string t = text_of(p, "dragonSoulText");
        return t.empty() ? 0 : std::atof(t.c_str());
    }
    bool shouting() override { return false; }
    std::string cast_failure(const std::string&) override { return {}; }
    std::string model(const Json& entry) override {
        // The capture of the 3D item manager: the model by the card's
        // name (an active effect's cardName, else the entry's text).
        const Json* c = m_ui.capture("item3d-models");
        const Json* paths = c ? member(c, "paths") : nullptr;
        const Json* models = paths ? member(paths, "MagicMenu") : nullptr;
        if (!models) {
            return {};
        }
        const std::string name = member(&entry, "cardName") ? text_of(&entry, "cardName") : text_of(&entry, "text");
        return text_of(models, name);
    }

    static constexpr int k_active_effects = 256;

private:
    struct Card {
        Json card;
        bool active;
    };
    const Json* find_spell(const std::string& text) const {
        for (const Json& s : m_spells) {
            if (text_of(&s, "text") == text && (int(number_of(&s, "filterFlag")) & k_active_effects) == 0) {
                return &s;
            }
        }
        return nullptr;
    }
    void load() {
        if (m_loaded) {
            return;
        }
        m_loaded = true;
        if (const Json* e =
                captured_path(m_ui, "magicmenu", "_root.Menu_mc.InventoryLists_mc._ItemsList.EntriesA");
            e && e->kind == Json::array) {
            m_spells = e->items;
        } else {
            log::info("ui: MagicMenu: no capture 'magicmenu'; "
                      "the lists stay empty");
        }
        if (const Json* p = captured_path(m_ui, "magicmenu", "_root.Menu_mc.BottomBar_mc.PlayerInfoObj")) {
            m_player = *p;
        }
        for (int i = 0;; ++i) {
            const std::string name = std::format("magicmenu-card-{}", i);
            const Json* card = captured_path(m_ui, name, "_root.Menu_mc.ItemCard_mc.LastUpdateObj");
            if (!m_ui.capture(name)) {
                break;
            }
            const Json* index =
                captured_path(m_ui, name, "_root.Menu_mc.InventoryLists_mc._ItemsList.iSelectedIndex");
            if (!card || card->kind != Json::object || !index || index->kind != Json::number) {
                continue;
            }
            const size_t at = size_t(index->n);
            const bool active =
                at < m_spells.size() && (int(number_of(&m_spells[at], "filterFlag")) & k_active_effects) != 0;
            m_cards.push_back({*card, active});
        }
        if (const Json* f =
                captured_path(m_ui, "favoritesmenu", "_root.MenuHolder.Menu_mc.ItemList.EntriesA");
            f && f->kind == Json::array) {
            m_favorites = f->items;
        }
    }

    Ui& m_ui;
    bool m_loaded = false;
    bool m_equip_logged = false;
    std::vector<Json> m_spells;
    std::vector<Card> m_cards;
    std::vector<Json> m_favorites;
    Json m_player;
};

// The running game's spells: what the host knows, the rest as captured.
class LiveMagic final : public MagicSource {
public:
    LiveMagic(Ui& ui, CapturedMagic& captured) : m_ui(ui), m_captured(captured) {}

    const std::vector<Json>& spells() override {
        m_rows = m_ui.host().ui_magic_rows();
        m_entries.clear();
        for (const MagicRow& r : m_rows) {
            Json e;
            e.kind = Json::object;
            Json text;
            text.kind = Json::string;
            text.s = r.text;
            set_member(e, "text", text);
            set_member(e, "count", number_json(1));
            set_member(e, "equipState", number_json(r.equip_state));
            set_member(e, "filterFlag", number_json(r.filter_flag));
            set_member(e, "favorite", number_json(0));
            Json yes;
            yes.kind = Json::boolean;
            yes.b = true;
            set_member(e, "enabled", yes);
            m_entries.push_back(std::move(e));
        }
        return m_entries;
    }
    const Json* item_card(const Json&) override { return nullptr; }
    const Json* player_info() override { return m_captured.player_info(); }
    std::vector<Json> favorites() override { return {}; }
    void toggle_favorite(const std::string&) override {}
    bool equip(const std::string& text, int slot) override {
        const auto row = std::ranges::find(m_rows, text, &MagicRow::text);
        return row != m_rows.end() && m_ui.host().ui_magic_equip(row->form, slot);
    }
    bool has_word_to_unlock(const std::string&) override { return false; }
    double dragon_souls() override { return m_captured.dragon_souls(); }
    bool shouting() override { return false; }
    std::string cast_failure(const std::string&) override { return {}; }
    std::string model(const Json&) override { return {}; }

private:
    Ui& m_ui;
    CapturedMagic& m_captured;
    std::vector<MagicRow> m_rows;
    std::vector<Json> m_entries;
};

MagicSource& magic_source(Ui& ui) {
    // The game keeps this data in its systems for the whole session; so does the source.
    static Ui* owner = nullptr;
    static std::unique_ptr<CapturedMagic> source;
    static std::unique_ptr<LiveMagic> live;
    if (!source || owner != &ui) {
        source = std::make_unique<CapturedMagic>(ui);
        live = std::make_unique<LiveMagic>(ui, *source);
        owner = &ui;
    }
    if (!ui.host().ui_magic_rows().empty()) {
        return *live;
    }
    return *source;
}

// ---- the Tween Menu's camera ---------------------------------------------------------------------------

// The game (vtable): while the Tween Menu is open the world camera turns a little
// towards the highlighted menu and its field of view widens; closing turns it back. The engine updates it
// from the Tween Menu's display function (PlayerCamera::Update) with the unscaled frame time.
struct TweenCamera {
    static constexpr float k_rot_angle = 0.05f;     // fTweenCamRotAngle:Camera (radians)
    static constexpr float k_rot_speed = 4.0f;      // fTweenCamRotSpeed:Camera
    static constexpr float k_closing_speed = 10.0f; // fTweenCamRotClosingSpeed:Camera
    static constexpr float k_zoom_fov = 10.0f;      // fTweenCamZoomFOVMod:Camera (degrees)
    static constexpr float k_zoom_speed = 25.0f;    // fTweenCamZoomSpeed:Camera (degrees per second)

    bool active = false;
    bool closing = false;
    float pitch = 0, yaw = 0;       // current turn (+52, +56); pitch positive looks down, yaw positive right
    float pitch_to = 0, yaw_to = 0; // target (+60, +64)
    float fov = 0, fov_to = 0;      // field-of-view offset and its target (+68, +72)

    void begin() {
        *this = {};
        active = true;
        fov_to = k_zoom_fov;
    }
    void
    highlight(int menu) { // 0 none, 1 skills (up), 2 magic (left), 3 items (right), 4 map (down)
        switch (menu) {
        case 0:
            pitch_to = 0;
            yaw_to = 0;
            break;
        case 1:
            pitch_to = -k_rot_angle;
            yaw_to = 0;
            break;
        case 2:
            pitch_to = 0;
            yaw_to = -k_rot_angle;
            break;
        case 3:
            pitch_to = 0;
            yaw_to = k_rot_angle;
            break;
        case 4:
            pitch_to = k_rot_angle;
            yaw_to = 0;
            break;
        default:
            break;
        }
    }
    void close() {
        if (!closing) {
            pitch_to = yaw_to = 0;
            fov_to = 0;
            closing = true;
        }
    }
    void update(float dt) {
        if (!active) {
            return;
        }
        if (pitch != pitch_to || yaw != yaw_to) {
            const float dp = pitch_to - pitch, dy = yaw_to - yaw;
            const float rate = closing ? k_closing_speed : k_rot_speed;
            const float sp = dp * dt * rate, sy = dy * dt * rate;
            if (sp * sp + sy * sy <= dp * dp + dy * dy) {
                pitch += sp;
                yaw += sy;
            } else {
                pitch = pitch_to;
                yaw = yaw_to;
            }
        }
        if (fov != fov_to) {
            const float step = k_zoom_speed * dt;
            if (std::fabs(step) <= std::fabs(fov_to - fov)) {
                fov += fov_to >= fov ? step : -step;
            } else {
                fov = fov_to;
            }
        }
        if (closing && fov == fov_to) {
            active = false; // the state pops
        }
    }
};

TweenCamera& tween_camera() {
    static TweenCamera camera;
    return camera;
}

// The Magic Menu's remembered list positions (SaveIndices): the category and each category's item,
// kept by the engine in globals for the session ().
struct MagicIndices {
    int category = -1;
    std::vector<int> items;
};

MagicIndices& magic_indices() {
    static MagicIndices indices;
    return indices;
}

// The Favorites Menu's remembered selection (FadeDone): index and the entry it pointed at.
struct FavoritesSelection {
    int index = 0;
    std::string text; // empty: none
};

FavoritesSelection& favorites_selection() {
    static FavoritesSelection s;
    return s;
}

void play(Ui& ui, const std::string& sound) {
    ui.host().ui_play_sound(sound);
}

// Script errors a movie reports while the menu runs (the UI logs only those of the first frame).
void log_new_errors(Menu& m, size_t& seen) {
    if (!m.player()) {
        return;
    }
    const std::vector<std::string> errors = m.player()->errors();
    for (size_t i = seen; i < errors.size(); ++i) {
        log::warn("ui {}: {}", m.name(), errors[i]);
    }
    seen = std::max(seen, errors.size());
}

// A HUD notification with a sound. The HUD Menu draws the text; it belongs to the HUD group.
void notify(Ui& ui, const std::string& text, const std::string& sound) {
    log::info("ui: notification: {}", text);
    if (!sound.empty()) {
        play(ui, sound);
    }
}

class TweenMenu;

// ---- Magic Menu (movie MagicMenu) ----------------------------------------------------------------------

class MagicMenu final : public Menu {
public:
    // Constructor: ExactFit, background alpha 0, depth 0, flags 0xA489, input context 3 (Item
    // Menus).
    explicit MagicMenu(Ui& ui)
        : Menu(ui, {"MagicMenu", "MagicMenu", ScaleMode::exact_fit, 0, 0xA489, 0, 3}),
          m_data(magic_source(ui)), m_item3d(ui) {}

    void on_show() override {
        // Show (message 1): the 3D item view in slot 2 at fMagic3DItemPos*, not interactive and not
        // normalised (Inventory3DManager::Begin(2), and cleared, SetBasePosition), then the categories,
        // the entries, the bottom bar and the remembered positions.
        m_item3d.begin(2);
        m_item3d.magic();
        m_stale = false;
        set_categories();
        populate();
        update_player_info();
        restore_indices();
    }

    bool on_user_event(const std::string& e) override {
        // Message type 7: the user events the game posts for a mapped button press.
        if (e == "YButton") {
            // Favourite or unfavourite the selected spell, power or shout (not an active effect).
            if (const Value entry = selected(); entry.is_object() && (filter_flag(entry) & 0x100) == 0) {
                m_data.toggle_favorite(text(entry));
                play(m_ui, "UIFavorite");
                inventory_changed();
            }
        } else if (e == "XButton") {
            // Spend a dragon soul on the selected shout's next word.
            const Value entry = selected();
            if (entry.is_object() && is_shout(entry) && m_data.has_word_to_unlock(text(entry))) {
                if (m_data.dragon_souls() <= 0) {
                    m_ui.message(m_ui.game_setting("sFailSpendSoul"), {m_ui.game_setting("sOk")});
                } else {
                    m_ui.message(m_ui.game_setting("sConfirmSpendSoul"),
                                 {m_ui.game_setting("sOk"), m_ui.game_setting("sCancel")});
                }
            }
        } else if (e == "Quick Magic") {
            press_tab(); // the movie closes on Tab
        } else if (e == "LeftEquip") {
            send("AttemptEquip", {Value::number(1)});
        } else if (e == "RightEquip") {
            send("AttemptEquip", {Value::number(0)});
        } else {
            return false;
        }
        return true;
    }

    void advance(double dt, double engine_ms) override {
        Menu::advance(dt, engine_ms);
        log_new_errors(*this, m_errors);
        m_item3d.update(static_cast<float>(dt)); // Inventory3DManager::Render, from MagicMenu::Display
    }

    // MagicMenu::Display: slot 2 first, then the movie over it.
    bool ui_scene(UiScene& out) override { return m_item3d.scene(out); }
    bool scene_after_movie() const override { return false; }
    std::string item3d_json() const { return m_item3d.state_json(); }

    // An inventory update for the player (message type 8): the entries and the bottom bar again.
    void inventory_changed() {
        m_stale = true;
        populate();
        update_player_info();
    }

private:
    void on_loaded() override {
        // The constructor keeps Menu_mc and notes whether it has bPCControlsReady (true in the PC movie).
        const Value menu = player()->get_path("Menu_mc");
        m_pc_controls_ready = menu.is_object() && !player()->vm.get(menu, "bPCControlsReady").is_undefined();
    }

    void register_handlers() override {
        // The movie's myLog calls: the engine registers no such callback and the game ignores unknown
        // names.
        handle("myLog", [](Call&) {});
        handle("PlaySound", [this](Call& c) {
            if (c.args.size() == 1 && c.arg(0).is_string()) {
                play(m_ui, *player()->vm.to_string(c.arg(0)));
            }
        });
        handle("UpdateItem3D", [this](Call& c) {
            // The game: true, the selected entry's form in the item view
            // false, clear it (Clear3D).
            if (player()->vm.to_boolean(c.arg(0))) {
                if (const Value entry = selected(); entry.is_object()) {
                    m_item3d.show(m_data.model(json_of(entry)));
                }
            } else {
                m_item3d.clear();
            }
        });
        handle("RequestItemCardInfo", [this](Call& c) {
            const Value entry = selected();
            if (!entry.is_object()) {
                return;
            }
            auto& vm = player()->vm;
            Value card;
            if (const Json* j = m_data.item_card(json_of(entry))) {
                card = engine_value(vm, *j);
            } else {
                // No card data: the members the handler itself copies from the entry.
                card = Value::object(vm.make_object());
                const Value name = vm.get(entry, "cardName");
                vm.set(card, "name", name.is_undefined() ? vm.get(entry, "text") : name);
                vm.set(card, "negativeEffect", vm.get(entry, "negativeEffect"));
                vm.set(card, "timeRemaining", vm.get(entry, "timeRemaining"));
            }
            c.respond({card});
        });
        handle("CloseMenu", [this](Call&) { m_ui.close(name()); });
        handle("ShowTweenMenu", [this](Call&) { tween_update(); });
        handle("CloseTweenMenu", [this](Call&) {
            tween_camera().close();
            m_ui.close("TweenMenu");
        });
        handle("ItemSelect", [this](Call& c) {
            // Without bPCControlsReady or a slot number the equip manager picks the hand; 0 right, 1 left.
            int slot = 0;
            if (m_pc_controls_ready && !c.args.empty() && c.arg(0).is_number()) {
                const double n = c.arg(0).n;
                if (n != 0.0 && n != 1.0) {
                    return;
                }
                slot = n == 0.0 ? 1 : 2;
            }
            equip(slot);
        });
        handle("SaveIndices", [this](Call& c) {
            // [category, item index of each category]
            const Value a = c.arg(0);
            if (!a.is_object()) {
                return;
            }
            auto& vm = player()->vm;
            const auto& el = a.o->elements;
            MagicIndices& s = magic_indices();
            s.category = el.empty() ? 0 : int(vm.to_number(el[0]));
            s.items.clear();
            for (size_t i = 1; i < el.size(); ++i) {
                s.items.push_back(int(vm.to_number(el[i])));
            }
        });
        handle("DisabledItemSelect", [this](Call&) {
            const Value entry = selected();
            if (!entry.is_object()) {
                return;
            }
            // A shout: sCannotCastShout; a spell: why the player can't cast it. Both with UIMenuCancel.
            if (is_shout(entry)) {
                notify(m_ui, m_ui.game_setting("sCannotCastShout"), "UIMenuCancel");
            } else {
                notify(m_ui, m_data.cast_failure(text(entry)), "UIMenuCancel");
            }
        });
        handle("ShowShoutFail", [this](Call&) {
            const Value entry = selected();
            if (!entry.is_object()) {
                return;
            }
            if (is_shout(entry)) {
                shout_fail();
            } else {
                notify(m_ui, m_data.cast_failure(text(entry)), "UIMenuCancel");
            }
        });
    }

    // SetCategoriesList: text, flag, dontHide for each category, then Menu_mc.hideButtonFlag =
    // 256 (the active effects hide the buttons).
    void set_categories() {
        static constexpr std::pair<const char*, int> k_categories[] = {
            {"sFavorites", 1},   {"sAllItems", -257},    {"sAlteration", 2},   {"sIllusion", 4},
            {"sDestruction", 8}, {"sConjuration", 16},   {"sRestoration", 32}, {"sShouts", 64},
            {"sPowers", 128},    {"sActiveEffects", 256}};
        std::vector<Value> args;
        for (const auto& [gmst, flag] : k_categories) {
            args.push_back(Value::string(m_ui.game_setting(gmst)));
            args.push_back(Value::number(flag));
            args.push_back(Value::boolean(flag == -257));
        }
        send("SetCategoriesList", std::move(args));
        player()->vm.set(player()->get_path("Menu_mc"), "hideButtonFlag", Value::number(256));
    }

    // The entries: the list's entryList emptied and refilled with the engine's objects, then
    // InvalidateListData.
    void populate() {
        auto& vm = player()->vm;
        const Value list = invoke("GetInventoryItemList");
        const Value entries = list.is_object() ? vm.get(list, "entryList") : Value{};
        if (entries.is_object()) {
            entries.o->elements.clear();
            for (const Json& e : m_data.spells()) {
                entries.o->elements.push_back(
                    engine_value(vm, e,
                                 {"text", "count", "equipState", "filterFlag", "favorite", "enabled",
                                  "cardName", "strEffects", "timeRemaining", "negativeEffect"}));
            }
        }
        m_stale = false;
        send("InvalidateListData");
    }

    void update_player_info() {
        auto& vm = player()->vm;
        const Json* p = m_data.player_info();
        send("UpdatePlayerInfo", {p ? engine_value(vm, *p) : Value::object(vm.make_object())});
    }

    void restore_indices() {
        const MagicIndices& s = magic_indices();
        std::vector<Value> args{Value::number(s.category)};
        for (int i : s.items) {
            args.push_back(Value::number(i));
        }
        send("RestoreIndices", std::move(args));
    }

    // The entry at the list's selectedIndex, none while the list waits for an update.
    Value selected() {
        if (m_stale || !player()) {
            return {};
        }
        auto& vm = player()->vm;
        const Value list = invoke("GetInventoryItemList");
        if (!list.is_object()) {
            return {};
        }
        const Value index = vm.get(list, "selectedIndex");
        const Value entries = vm.get(list, "entryList");
        if (!index.is_number() || !entries.is_object() || index.n < 0 ||
            index.n >= double(entries.o->elements.size())) {
            return {};
        }
        return entries.o->elements[size_t(index.n)];
    }

    std::string text(const Value& entry) {
        return entry.is_object() ? *player()->vm.to_string(player()->vm.get(entry, "text")) : std::string();
    }
    int filter_flag(const Value& entry) {
        const Value f = player()->vm.get(entry, "filterFlag");
        return f.is_number() ? int(f.n) : 0;
    }
    Json json_of(const Value& entry) {
        Json j;
        j.kind = Json::object;
        auto& vm = player()->vm;
        for (const char* k : {"text", "cardName", "filterFlag"}) {
            const Value v = vm.get(entry, k);
            if (v.is_string()) {
                Json s;
                s.kind = Json::string;
                s.s = *vm.to_string(v);
                j.members.emplace_back(k, std::move(s));
            } else if (v.is_number()) {
                j.members.emplace_back(k, number_json(v.n));
            }
        }
        return j;
    }

    void equip(int slot) {
        const Value entry = selected();
        if (!entry.is_object()) {
            return;
        }
        const Value enabled = player()->vm.get(entry, "enabled");
        const bool on = enabled.type == player::Type::boolean ? enabled.b : true;
        if (on) {
            if ((filter_flag(entry) & 0x100) == 0) {
                m_data.equip(text(entry), slot);
                inventory_changed();
            }
        } else if (is_shout(entry)) {
            shout_fail();
        }
    }

    // A shout entry (the engine tests the form type, SHOU; its entries carry the SHOUTS category, 64).
    bool is_shout(const Value& entry) { return (filter_flag(entry) & 64) != 0; }

    // A disabled shout: while shouting, sFailShouting as a notification; otherwise the sFailSpendSoul box.
    void shout_fail() {
        if (m_data.shouting()) {
            notify(m_ui, m_ui.game_setting("sFailShouting"), {});
        } else {
            m_ui.message(m_ui.game_setting("sFailSpendSoul"), {m_ui.game_setting("sOk")});
        }
    }

    void press_tab() {
        if (player()) {
            player()->key(9, 0, true); // a key-down of Tab straight to the movie
        }
    }

    void tween_update();

    MagicSource& m_data;
    // The constructor puts the HUD into "InventoryMode"; the destructor ends it (the game
    //).
    HudModeHold m_hud_mode{m_ui, "InventoryMode"};
    BlurHold m_blur{m_ui};
    bool m_pc_controls_ready = false;
    bool m_stale = false;
    Item3D m_item3d;
    size_t m_errors = 0;
};

// ---- Favorites Menu (movie FavoritesMenu) --------------------------------------------------------------

class FavoritesMenu final : public Menu {
public:
    // Constructor: ExactFit, background alpha 0, depth 0, flags 0xA401, input context 6
    // (Favorites).
    explicit FavoritesMenu(Ui& ui)
        : Menu(ui, {"FavoritesMenu", "FavoritesMenu", ScaleMode::exact_fit, 0, 0xA401, 0, 6}),
          m_data(magic_source(ui)) {}

    void on_show() override {
        // Show (message 1): the entries; with none the menu does not stay (and the cursor goes);
        // with a remembered selection, SetSelectedItem.
        if (!populate()) {
            m_ui.close_now(name());
            m_ui.close("Cursor Menu");
            return;
        }
        if (!favorites_selection().text.empty()) {
            set_selected_item();
        }
    }

    void advance(double dt, double engine_ms) override {
        Menu::advance(dt, engine_ms);
        log_new_errors(*this, m_errors);
    }

    bool on_user_event(const std::string& e) override {
        // Message type 7: the attack buttons equip the selected favourite in that hand.
        if (m_equipping) {
            return false;
        }
        if (e == "Left Attack/Block") {
            equip(2);
        } else if (e == "Right Attack/Block") {
            equip(1);
        } else {
            return false;
        }
        return true;
    }

private:
    void on_loaded() override {
        const Value menu = player()->get_path("MenuHolder.Menu_mc");
        m_pc_controls_ready = menu.is_object() && !player()->vm.get(menu, "bPCControlsReady").is_undefined();
    }

    void register_handlers() override {
        handle("myLog", [](Call&) {}); // not registered by the engine; the game ignores unknown names
        handle("FadeDone", [this](Call& c) {
            // The movie faded out: hide, and remember the selection it reports.
            m_ui.close(name());
            if (c.args.size() == 1 && c.arg(0).is_number()) {
                const int index = std::max(0, int(c.arg(0).n));
                FavoritesSelection& s = favorites_selection();
                s.index = index;
                s.text = size_t(index) < m_entries.size() ? text_of(&m_entries[size_t(index)], "text")
                                                          : std::string();
            }
        });
        handle("PlaySound", [this](Call& c) {
            if (c.args.size() == 1 && c.arg(0).is_string()) {
                play(m_ui, *player()->vm.to_string(c.arg(0)));
            }
        });
        handle("ItemSelect", [this](Call& c) {
            int slot = 0;
            if (!c.args.empty() && c.arg(0).is_number()) {
                const double n = c.arg(0).n;
                if (n != 0.0 && n != 1.0) {
                    return;
                }
                slot = n == 0.0 ? 1 : 2;
            }
            equip(slot);
        });
    }

    // MenuHolder.Menu_mc.ItemList.entryList refilled, then InvalidateData (sortOn("text") and
    // UpdateList without bPCControlsReady). True when there are entries.
    bool populate() {
        auto& vm = player()->vm;
        const Value list = player()->get_path("MenuHolder.Menu_mc.ItemList");
        const Value entries = list.is_object() ? vm.get(list, "entryList") : Value{};
        ItemSource* items = m_ui.host().ui_item_source();
        m_entries = items ? items->favorites() : m_data.favorites();
        if (!entries.is_object()) {
            return !m_entries.empty();
        }
        entries.o->elements.clear();
        for (const Json& e : m_entries) {
            entries.o->elements.push_back(engine_value(vm, e, {"text", "index", "equipState", "hotkey"}));
        }
        if (!m_entries.empty()) {
            if (m_pc_controls_ready) {
                vm.call_method(list, "InvalidateData", {});
            } else {
                vm.call_method(entries, "sortOn", {Value::string("text")});
                vm.call_method(list, "UpdateList", {});
            }
        }
        m_equipping = false;
        return !m_entries.empty();
    }

    void set_selected_item() {
        const FavoritesSelection& s = favorites_selection();
        const int n = int(m_entries.size());
        int index = -1;
        if (s.index < n && text_of(&m_entries[size_t(s.index)], "text") == s.text) {
            index = s.index;
        } else {
            for (int i = 0; i < n && index < 0; ++i) {
                if (text_of(&m_entries[size_t(i)], "text") == s.text) {
                    index = i;
                }
            }
            if (index < 0) {
                index = n <= s.index ? n - 1 : s.index;
            }
        }
        send("SetSelectedItem", {Value::number(index)});
    }

    void equip(int slot) { // the entry at _root.GetSelectedIndex
        const Value index = invoke("GetSelectedIndex");
        if (!index.is_number() || index.n == -1.0 || m_entries.empty() || index.n < 0 ||
            index.n >= double(m_entries.size())) {
            return;
        }
        if (ItemSource* items = m_ui.host().ui_item_source()) {
            items->equip_favorite(int(index.n), slot - 1);
        } else {
            m_data.equip(text_of(&m_entries[size_t(index.n)], "text"), slot);
        }
        play(m_ui, "UIFavorite");
        m_equipping = true; // until the inventory update repopulates
        if (!populate()) {
            send("StartFadeOut");
        }
    }

    MagicSource& m_data;
    std::vector<Json> m_entries;
    bool m_pc_controls_ready = false;
    bool m_equipping = false;
    size_t m_errors = 0;
};

// ---- Tween Menu (movie TweenMenu) ----------------------------------------------------------------------

class TweenMenu final : public Menu {
public:
    // Constructor: ExactFit, background alpha 0, depth 0, flags 0xCC01, input context 1 (Menu
    // Mode).
    explicit TweenMenu(Ui& ui)
        : Menu(ui, {"TweenMenu", "TweenMenu", ScaleMode::exact_fit, 0, 0xCC01, 0, 1}) {}

private:
    // Show puts the HUD into "TweenMode" (TweenMenu::ProcessMessage); the destructor ends it
    HudModeHold m_hud_mode{m_ui, "TweenMode"};
    // Show takes the blur count, the Map Menu gives it up; an update message takes it again if
    // nothing holds it.
    BlurHold m_blur{m_ui};

public:

    void on_show() override {
        // Show (message 1): the camera's tween state, the HUD's TweenMode, then StartOpenMenuAnim.
        tween_camera().begin();
        start_open_anim();
    }
    bool on_hide() override {
        // Hide (message 3): the camera leaves the tween state.
        if (!tween_camera().closing) {
            tween_camera() = {};
        }
        return true;
    }
    void advance(double dt, double engine_ms) override {
        Menu::advance(dt, engine_ms);
        // Display: PlayerCamera::Update, which moves the tween state.
        tween_camera().update(float(dt));
        log_new_errors(*this, m_errors);
    }

    // An update message (type 0, ShowTweenMenu from the Magic Menu): the menu shows itself again.
    void on_update() {
        start_open_anim();
        if (m_ui.blur_count() == 0) {
            m_blur.set(true);
        }
    }

    bool on_user_event(const std::string& e) override {
        // Message types 7 and 12: the quick-menu keys open a menu as OpenHighlightedMenu does;
        // Tween Menu (Tab) is a Tab key-down for the movie, which closes.
        if (e == "Quick Stats") {
            open_highlighted(1);
        } else if (e == "Quick Magic") {
            open_highlighted(2);
        } else if (e == "Quick Inventory") {
            open_highlighted(3);
        } else if (e == "Quick Map") {
            open_highlighted(4);
        } else if (e == "Tween Menu") {
            player()->key(9, 0, true);
        } else {
            return false;
        }
        return true;
    }

private:
    void register_handlers() override {
        handle("myLog", [](Call&) {}); // not registered by the engine; the game ignores unknown names
        handle("OpenHighlightedMenu",
               [this](Call& c) { open_highlighted(int(player()->vm.to_number(c.arg(0)))); });
        handle("HighlightMenu",
               [this](Call& c) { tween_camera().highlight(int(int16_t(player()->vm.to_number(c.arg(0))))); });
        handle("StartCloseMenu", [](Call&) { tween_camera().close(); });
        handle("CloseMenu", [this](Call&) { m_ui.close(name()); });
        handle("OpenAnimFinished", [](Call&) {});
    }

    // StartOpenMenuAnim: level up available (and not survival mode), the date with the year, the
    // level, the progress to the next level in percent.
    void start_open_anim() {
        const PlayerInfo p = m_ui.host().ui_player_info();
        send("StartOpenMenuAnim",
             {Value::boolean(p.progress_percent >= 100.0), Value::string(m_ui.date_text(m_ui.calendar(p))),
              Value::number(p.level), Value::number(p.progress_percent)});
    }

    // 1 skills (StatsMenu, after a fade, and ResetStatsButton), 2 MagicMenu, 3 InventoryMenu, 4
    // MapMenu (which closes the Tween Menu); then HideMenu, a mouse-up for the movie and the camera turned to
    // the menu.
    void open_highlighted(int menu) {
        switch (menu) {
        case 1:
            // The game(1, 1, s, 1, 0): to black, and it stays: nothing but this black lies behind the
            // skydome's blended layers. s is 0.5 with the skydome loaded, as Show requested it.
            m_ui.request_fade({false, true, 0.5, 0.0, true});
            m_ui.open("StatsMenu");
            send("ResetStatsButton");
            break;
        case 2:
            m_ui.open("MagicMenu");
            break;
        case 3:
            m_ui.open("InventoryMenu");
            break;
        case 4:
            m_ui.open("MapMenu");
            m_blur.set(false);
            m_ui.close(name());
            break;
        default:
            break;
        }
        send("HideMenu");
        player()->mouse_button(player::MouseButton::left, false);
        tween_camera().highlight(menu);
    }

    size_t m_errors = 0;
};

void MagicMenu::tween_update() { tween_menu_update(m_ui); }

} // namespace

void tween_menu_update(Ui& ui) {
    if (Menu* t = ui.find_menu("TweenMenu")) {
        static_cast<TweenMenu*>(t)->on_update();
    }
}

std::string magic_item3d_json(Menu& m) {
    if (auto* mm = dynamic_cast<MagicMenu*>(&m)) {
        return mm->item3d_json();
    }
    return "null";
}

std::unique_ptr<Menu> create_magic_menu(Ui& ui, const std::string& name) {
    if (name == "MagicMenu")
        return std::make_unique<MagicMenu>(ui);
    if (name == "FavoritesMenu")
        return std::make_unique<FavoritesMenu>(ui);
    if (name == "TweenMenu")
        return std::make_unique<TweenMenu>(ui);
    return nullptr;
}

} // namespace spud::ui
