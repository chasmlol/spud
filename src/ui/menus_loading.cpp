// The Loading Menu and the Mist Menu it shows its 3D model in: the game's loading screens, implemented from
// (addresses there; the game). The Mist Menu is also the main menu's logo
// backdrop.

#include "ui/loading_screens.hpp"
#include "ui/menu.hpp"
#include "ui/menus_internal.hpp"
#include "ui/ui.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "fx/effect.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <numbers>
#include <optional>

namespace spud::ui {

namespace {

using Value = player::Value;

// the game's defaults for these settings (no INI or plugin of this install overrides them).
constexpr float k_camera[3] = {-50.0f, 600.0f, 80.0f};  // fUIMistMenu_Camera{X,Y,Z}_G
constexpr float k_look_at[3] = {-50.0f, 0.0f, 0.0f};    // fUIMistMenu_CameraLookAt{X,Y,Z}_G
constexpr float k_camera_fov = 75.0f;                   // fUIMistMenu_CameraFOV_G
constexpr float k_default_logo_scale = 1.0f;            // fUIMistMenu_DefaultLogoNIFScale
constexpr float k_alt_logo_translate[3] = {0, 0, 0};     // fUIAltLogoModel_Translate{X,Y,Z}_G
constexpr float k_zoom_min_fov = 60.0f;                 // fUIMistMenu_LogoOnscreenZoomMinFOV
constexpr float k_zoom_max_fov = 95.0f;                 // fUIMistMenu_LogoOnscreenZoomMaxFOV
constexpr float k_mouse_rotate = 0.02f;                 // fUILogoModel_MouseToRotateSpeed
constexpr float k_mouse_zoom = 0.6f;                    // fUILogoModel_MouseToZoomSpeed
constexpr float k_stick_rotate = 0.45f;                 // fUILogoModel_ThumbstickToRotateSpeed
constexpr float k_stick_zoom = 5.0f;                    // fUILogoModel_ThumbstickToZoomSpeed
constexpr float k_degree = 0.017453292f;                // the game's degrees-to-radians constant
constexpr bool k_use_all_non_default_first = false;     // bUseAllNonDefaultLoadScreensFirst:Interface
constexpr uint32_t k_char_gen_quest = 0x0003372B;       // sCharGenQuest:General ("0003372b")
constexpr uint16_t k_char_gen_stage_at_first_load = 10; // measured
constexpr uint8_t k_char_gen_flags_at_first_load = 0x81; // the same measurement

// NiMatrix3, row-major m[row][col], column vectors.
struct Mat3 {
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};

Mat3 mul(const Mat3& a, const Mat3& b) { // a * b
    Mat3 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        }
    }
    return r;
}

// The game: Rx * Ry * Rz of the engine's elementary matrices, which turn by the
// negative angle in the column-vector convention.
Mat3 euler_xyz(float x, float y, float z) {
    const float sx = std::sin(x), cx = std::cos(x), sy = std::sin(y), cy = std::cos(y);
    const float sz = std::sin(z), cz = std::cos(z);
    Mat3 rx, ry, rz;
    rx.m[1][1] = cx, rx.m[1][2] = sx, rx.m[2][1] = -sx, rx.m[2][2] = cx;
    ry.m[0][0] = cy, ry.m[0][2] = -sy, ry.m[2][0] = sy, ry.m[2][2] = cy;
    rz.m[0][0] = cz, rz.m[0][1] = sz, rz.m[1][0] = -sz, rz.m[1][1] = cz;
    return mul(rx, mul(ry, rz));
}

// The game's first step ( then): the quaternion of Euler angles (0, 0, pi) written as
// the root's rotation, a half turn about Z.
Mat3 half_turn_z() {
    const float h = std::numbers::pi_v<float> * 0.5f;
    const float w = std::cos(h), z = std::sin(h); // x = y = 0
    Mat3 r;
    r.m[0][0] = 1.0f - (z * (z + z));
    r.m[0][1] = -(w * (z + z));
    r.m[1][0] = w * (z + z);
    r.m[1][1] = 1.0f - (z * (z + z));
    return r;
}

// The game with up (0, 0, 1): the camera's axes are its view direction (X), up (Y) and right
// (Z), the columns of its world rotation.
Mat3 look_at(const float (&eye)[3], const float (&target)[3]) {
    glm::vec3 d = glm::normalize(glm::vec3(target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]));
    glm::vec3 right = glm::normalize(glm::cross(d, glm::vec3(0, 0, 1)));
    glm::vec3 up = glm::cross(right, d);
    Mat3 r;
    for (int i = 0; i < 3; ++i) {
        r.m[i][0] = d[i];
        r.m[i][1] = up[i];
        r.m[i][2] = right[i];
    }
    return r;
}

// What the game is given for a loading screen.
struct ShownModel {
    std::string model;       // cache .osm path; empty: none
    float scale = 1.0f;      // SNAM
    float euler[3] = {0, 0, 0}; // RNAM, radians
    float offset[3] = {0, 0, 0}; // XNAM
    bool inv_marker = false; // the model's root has a BSInvMarker "INV"
    float inv_euler[3] = {0, 0, 0};
    std::string camera_path; // cache .ose path; empty: none
    float rotation_min = -180.0f, rotation_max = 180.0f; // ONAM degrees
};

// ---- Mist Menu: the 3D backdrop, no movie ------------------------------------------------------------------------

class MistMenu final : public Menu {
public:
    explicit MistMenu(Ui& ui) : Menu(ui, {"Mist Menu", "", ScaleMode::show_all, 8, 0x4880}) {
        m_model = ui.backdrop_logo();
        place(ShownModel{});
    }

    // only while no model was shown yet or the default logo is up.
    void show_model(const ShownModel& s) {
        if (m_shown && !m_default) {
            return;
        }
        m_model = s.model;
        m_default = false;
        m_shown = true;
        m_rotation_min = s.rotation_min;
        m_rotation_max = s.rotation_max;
        place(s);
    }

    bool default_logo() const { return m_default; }

    void advance(double dt, double) override {
        // AdvanceMovie: the interval for the inspection controls, the camera path at the UI clock.
        m_frame_seconds = static_cast<float>(dt);
        if (m_path && m_path_node != ose::k_none) {
            const uint32_t now = m_ui.clock_ms();
            if (m_path_restart) {
                m_path_start = now;
                m_path_restart = false;
            }
            m_path->update(static_cast<float>(now - m_path_start) * 0.001f);
            place_camera_on_path();
        }
    }

    void scene(BackdropScene& s) const {
        s.model = m_default ? m_ui.backdrop_logo() : m_model; // the Main Menu swaps the default after this menu opens
        s.default_logo = m_default;
        for (int i = 0; i < 3; ++i) {
            s.model_translation[i] = m_translation[i];
            s.camera[i] = m_camera[i];
            s.look_at[i] = k_look_at[i];
            for (int j = 0; j < 3; ++j) {
                s.model_rotation[i * 3 + j] = m_rotation.m[i][j];
                s.camera_rotation[i * 3 + j] = m_camera_rotation.m[i][j];
            }
        }
        s.model_scale = m_scale;
        s.camera_rotation_set = m_path != nullptr && m_path_node != ose::k_none;
        s.fov_degrees = m_fov;
    }

    // The menu's input handler takes input only for a loading screen's model during a load.
    bool can_process() const { return !m_default && m_ui.is_open("Loading Menu"); }

    // ProcessButton: mouse button 0 held turns the model with the mouse.
    void button(int b, bool down) {
        if (b == 0) {
            m_drag = down;
        }
    }
    // ProcessMouseMove: while button 0 is held, turn by dx and zoom by dy, scaled by the frame interval.
    bool mouse_move(int dx, int dy) {
        if (!m_drag) {
            return false;
        }
        rotate(static_cast<float>(dx) * m_frame_seconds * k_mouse_rotate);
        zoom_to(static_cast<float>(dy) * m_frame_seconds * k_mouse_zoom + m_fov);
        return true;
    }
    // ProcessThumbstick: either stick.
    void thumbstick(float x, float y) {
        rotate(-(k_stick_rotate * m_frame_seconds * x));
        zoom_to(m_fov - k_stick_zoom * m_frame_seconds * y);
    }

    float turned() const { return m_turned; }
    float fov() const { return m_fov; }
    float frame_seconds() const { return m_frame_seconds; }
    // The camera path's clock: (UI ms - start) x 0.001 at the last advance, -1 without a path.
    float path_seconds() const {
        return m_path && m_path_node != ose::k_none && !m_path_restart
                   ? static_cast<float>(m_ui.clock_ms() - m_path_start) * 0.001f
                   : -1.0f;
    }

private:
    // The game and the game.
    void place(const ShownModel& s) {
        const float aspect = float(m_ui.target_width()) / float(std::max(m_ui.target_height(), 1));
        const bool wide = aspect > 1.7777778f;
        const float k = 1.7777778f / aspect;
        float offset[3] = {s.offset[0], s.offset[1], s.offset[2]};
        if (wide) {
            for (float& o : offset) {
                o = k * o;
            }
        }
        for (int i = 0; i < 3; ++i) {
            m_translation[i] = k_alt_logo_translate[i] + offset[i];
        }
        Mat3 base = half_turn_z();
        if (s.inv_marker) {
            base = euler_xyz(s.inv_euler[0], s.inv_euler[1], s.inv_euler[2]);
        }
        m_rotation = mul(base, euler_xyz(s.euler[0], s.euler[1], s.euler[2]));
        float scale = m_default ? k_default_logo_scale : s.scale;
        if (wide) {
            scale = scale * k;
        }
        m_scale = std::fabs(scale);
        setup_camera(m_default ? std::string() : s.camera_path);
    }

    void setup_camera(const std::string& path) {
        for (int i = 0; i < 3; ++i) {
            m_camera[i] = k_camera[i];
        }
        m_camera_rotation = look_at(k_camera, k_look_at);
        m_camera_rotation0 = m_camera_rotation;
        m_fov = k_camera_fov;
        m_path.reset();
        m_path_node = ose::k_none;
        if (path.empty()) {
            return;
        }
        // The camera path NIF: its sequence "CameraPath" started, the root updated at 0.001 s, the camera placed
        // from "CameraPathNode"; the clock restarts at the next frame.
        try {
            const auto file = m_ui.interface_dir().parent_path() / path;
            m_path_effect = fx::load_effect(read_file(file));
            m_path = std::make_unique<fx::EffectInstance>(m_path_effect);
            m_path->activate_sequence("CameraPath", 1.0f, 0.0f, 0);
            m_path->update(0.001f);
            m_path_node = m_path->find_node("CameraPathNode");
            if (m_path_node != ose::k_none) {
                place_camera_on_path();
                m_path_restart = true;
            }
        } catch (const std::exception& e) {
            log::warn("ui: camera path {}: {}", path, e.what());
            m_path.reset();
        }
    }

    // (CameraX - node.x, CameraY - node.y, CameraZ + node.z), rotation node world rotation x the initial one.
    void place_camera_on_path() {
        const glm::vec3 t = m_path->node_world_translation_units(m_path_node);
        const glm::mat3 r = m_path->node_world_rotation(m_path_node);
        m_camera[0] = k_camera[0] - t.x;
        m_camera[1] = k_camera[1] - t.y;
        m_camera[2] = k_camera[2] + t.z;
        Mat3 node;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                node.m[i][j] = r[j][i]; // glm is column-major
            }
        }
        m_camera_rotation = mul(node, m_camera_rotation0);
    }

    // The game: a turn about Z, kept inside the ONAM limits unless they are the full circle.
    void rotate(float delta) {
        const float deg = (m_turned + delta) * 57.295776f;
        if (delta < 0.0f) {
            if (m_rotation_min > -180.0f && deg < m_rotation_min) {
                delta = m_rotation_min * k_degree - m_turned;
            }
        } else if (delta > 0.0f) {
            if (!(m_rotation_max >= 180.0f || deg <= m_rotation_max)) {
                delta = m_rotation_max * k_degree - m_turned;
            }
        }
        const float sn = std::sin(delta), cs = std::cos(delta);
        Mat3 r;
        r.m[0][0] = cs, r.m[0][1] = sn, r.m[1][0] = -sn, r.m[1][1] = cs;
        m_rotation = mul(r, m_rotation);
        m_turned = delta + m_turned;
    }

    // The game: the FOV only changes while it stays strictly inside (60, 95).
    void zoom_to(float fov) {
        if (fov > k_zoom_min_fov && fov < k_zoom_max_fov) {
            m_fov = fov;
        }
    }

    std::string m_model;
    bool m_default = true; // the default logo (g_MistMenuCurrentLogoPath == g_MistMenuDefaultLogoPath)
    bool m_shown = false;
    float m_translation[3] = {0, 0, 0};
    Mat3 m_rotation;
    float m_scale = 1.0f;
    float m_camera[3] = {0, 0, 0};
    Mat3 m_camera_rotation, m_camera_rotation0; // the rotation the look-at gave
    float m_fov = k_camera_fov;
    float m_turned = 0.0f;                     // reset only when the menu is made
    float m_rotation_min = -180.0f, m_rotation_max = 180.0f;
    float m_frame_seconds = 0.0f;
    bool m_drag = false;                       // of the input handler
    std::shared_ptr<const fx::Effect> m_path_effect;
    std::unique_ptr<fx::EffectInstance> m_path;
    uint32_t m_path_node = ose::k_none;
    bool m_path_restart = false;
    uint32_t m_path_start = 0;
};

// ---- Loading Menu -------------------------------------------------------------------------------------------------

class LoadingMenu final : public Menu {
public:
    explicit LoadingMenu(Ui& ui) : Menu(ui, {"Loading Menu", "LoadingMenu", ScaleMode::exact_fit, 9, 0x20803}) {}

    void on_show() override {
        // ProcessMessage, show: the Mist Menu without resetting its model, SetPlatform again, an instant
        // fade if the Fader Menu is not open (the game(0, 1, 0.0, 1, 0): the movie's initFade gets
        // true, true, 0, 0).
        m_ui.open("Mist Menu");
        if (has_path("SetPlatform")) {
            invoke("SetPlatform", {Value::number(m_ui.gamepad_active() ? 1 : 0), Value::boolean(false)});
        }
        if (!m_ui.is_open("Fader Menu")) {
            // The fade data rides with the Fader Menu's show message ( -> the game).
            m_ui.request_fade({true, true, 0.0, 0.0, true});
        }
        // The loading code posts the show with the loading data, processes it (the Mist Menu's show is only queued
        // then, so nothing is set up yet), and posts an update with the same data; the first message
        // with data that finds the Mist Menu open sets up the model and fades the menu in.
        m_destination = m_ui.host().ui_loading_destination();
        set_up();
    }
    void advance(double dt, double engine_ms) override {
        set_up();
        Menu::advance(dt, engine_ms);
    }
    bool on_hide() override {
        // The end of a load (hide): the world fades in from black,
        // The game(0, 1, 1.0, 0, 1.5) (the floats and), so initFade(true, true, 1,
        // 1.5), 149 updates as traced in the game. The game asks for the Fader Menu being up, and it was at the end of
        // every load measured, an 8 s one too. Here its instant fade ends while the screen draws, and it can't stay: the
        // backdrop lies behind the UI, so its black would cover the screen. Not after the main menu's New Game.
        if (!m_ui.take_new_game_load()) {
            m_ui.request_fade({true, true, 1.0, 1.5});
        }
        invoke("Menu_mc.FadeOutMenu");
        return true;
    }

    const LoadingScreens::Choice& choice() const { return m_choice; }
    uint32_t location() const { return m_location; }
    const osl::LoadScreen* shown() const { return m_shown; }

private:
    void register_handlers() override {
        handle("RequestLoadingText", [this](Call& c) {
            const LoadingScreens* screens = m_ui.loading_screens();
            std::optional<std::string> text;
            if (screens) {
                text = screens->next_text(m_choice, m_no_screen, k_use_all_non_default_first, m_ui.random());
            }
            if (text) {
                c.respond({Value::string(*text)});
            } else {
                c.respond({});
            }
        });
        handle("RequestPlayerInfo", [this](Call& c) {
            // level, progress in percent, and the meter's visibility (Main clear and the PC platform
            // query, which answers true).
            const PlayerInfo p = m_ui.host().ui_player_info();
            c.respond({Value::number(p.level), Value::number(p.progress_percent), Value::boolean(true)});
        });
    }

    void set_up() {
        Menu* mist_menu = m_ui.find_menu("Mist Menu");
        if (m_set_up || !mist_menu) {
            return;
        }
        const LoadingDestination& d = m_destination;
        auto& mist = static_cast<MistMenu&>(*mist_menu);
        m_no_screen = d.no_screen;
        const LoadingScreens* screens = m_ui.loading_screens();
        if (!screens) {
            log::info("ui: no loading screens in the cache (skytool ui import writes them)");
        }
        m_location = 0;
        if (d.location) {
            m_location = d.location;
        } else if (screens && !d.new_game) {
            // A new game's first load comes from the session start, which posts no location (measured:
            // SetupModel saw none).
            m_location = d.interior.empty() ? screens->location_at(d.worldspace, d.cell_x, d.cell_y)
                                            : screens->interior_location(d.interior);
        }
        if (m_no_screen) {
            // The game with the no-screen flag: no model, no camera path.
            mist.show_model(ShownModel{});
        } else if (screens) {
            StartingState state(screens->data());
            if (d.new_game) {
                // StartNewGame starts the char-gen quest before the load. Measured at that load's SetupModel: MQ101
                // running (flags 0x81) at stage 10, which its stage-0 Papyrus fragment sets (the record's start-up
                // stage is 0); spud runs no Papyrus, so the measured stage stands in.
                state.start_quest(k_char_gen_quest, k_char_gen_stage_at_first_load);
                state.set_quest_flags(k_char_gen_quest, k_char_gen_flags_at_first_load);
            }
            // Automation: a state measured in the game (ui loadfor ... quest / global).
            for (const LoadingDestination::QuestState& q : d.quests) {
                state.set_quest_stage(q.quest, q.stage);
                state.set_quest_flags(q.quest, q.flags);
            }
            for (const auto& [global, value] : d.globals) {
                state.set_global(global, value);
            }
            ConditionContext ctx;
            ctx.location = m_location;
            ctx.player_worldspace = screens->worldspace(d.from_worldspace);
            ctx.state = &state;
            m_rng_before = m_ui.random().index();
            m_choice = screens->choose(ctx, m_ui.random());
            m_rng_after = m_ui.random().index();
            if (const uint32_t forced = m_ui.take_forced_loading_screen()) {
                // Automation ("ui loadscreen <form ID>"): this screen instead of the pick, for comparisons.
                if (const osl::LoadScreen* f = screens->find(forced)) {
                    if (m_choice.chosen && m_choice.chosen != f) {
                        m_choice.eligible.push_back(m_choice.chosen);
                    }
                    std::erase(m_choice.eligible, f);
                    m_choice.chosen = f;
                }
            }
            m_conditioned_at_setup = m_choice.conditioned;
            if (const osl::LoadScreen* s = m_choice.chosen) {
                m_shown = s;
                mist.show_model(model_of(*screens, *s));
                log::info("ui: loading screen {} ({:08X}) at {} ({:08X}), {} eligible", screens->edid(*s), s->form_id,
                          screens->location_edid(m_location), m_location, m_choice.eligible.size() + 1);
            }
        }
        invoke("Menu_mc.FadeInMenu");
        m_set_up = true;
    }

    static ShownModel model_of(const LoadingScreens& screens, const osl::LoadScreen& s) {
        ShownModel m;
        const auto& data = screens.data();
        m.model = std::string(data.string(s.model));
        m.scale = s.scale;
        for (int i = 0; i < 3; ++i) {
            // (float)(int16) x 0.017453292.
            m.euler[i] = static_cast<float>(s.rotation[i]) * k_degree;
            m.offset[i] = s.offset[i];
            m.inv_euler[i] = s.inv_rotation[i];
        }
        m.inv_marker = s.has_inv_marker != 0;
        m.camera_path = std::string(data.string(s.camera_path));
        m.rotation_min = static_cast<float>(s.rotation_min);
        m.rotation_max = static_cast<float>(s.rotation_max);
        return m;
    }

    LoadingDestination m_destination; // the loading data
    bool m_set_up = false;
    bool m_no_screen = false;
    uint32_t m_location = 0;
    LoadingScreens::Choice m_choice;
    const osl::LoadScreen* m_shown = nullptr;
    uint32_t m_conditioned_at_setup = 0;
    uint32_t m_rng_before = 0, m_rng_after = 0; // The game's index around the choice

public:
    uint32_t conditioned_at_setup() const { return m_conditioned_at_setup; }
    uint32_t rng_before() const { return m_rng_before; }
    uint32_t rng_after() const { return m_rng_after; }
};

} // namespace

std::unique_ptr<Menu> create_loading_menu(Ui& ui, const std::string& name) {
    if (name == "Mist Menu") return std::make_unique<MistMenu>(ui);
    if (name == "Loading Menu") return std::make_unique<LoadingMenu>(ui);
    return nullptr;
}

// Hooks for Ui (ui.cpp).
void mist_scene(Menu& m, BackdropScene& s) {
    static_cast<MistMenu&>(m).scene(s);
}
bool mist_can_process(Menu& m) {
    return static_cast<MistMenu&>(m).can_process();
}
void mist_button(Menu& m, int button, bool down) {
    static_cast<MistMenu&>(m).button(button, down);
}
bool mist_mouse_move(Menu& m, int dx, int dy) {
    return static_cast<MistMenu&>(m).mouse_move(dx, dy);
}
void mist_thumbstick(Menu& m, float x, float y) {
    static_cast<MistMenu&>(m).thumbstick(x, y);
}
std::string loading_state_json(Menu& loading, Menu* mist) {
    auto& l = static_cast<LoadingMenu&>(loading);
    const LoadingScreens* screens = loading.ui().loading_screens();
    std::string j = std::format("{{\"location\": \"{:08X}\"", l.location());
    if (screens) {
        j += std::format(", \"location_edid\": \"{}\"", screens->location_edid(l.location()));
    }
    if (const osl::LoadScreen* s = l.shown()) {
        j += std::format(", \"chosen\": \"{:08X}\"", s->form_id);
        if (screens) {
            std::string text;
            for (char ch : screens->text(*s)) {
                if (ch == '"' || ch == '\\') {
                    text += '\\';
                    text += ch;
                } else if (ch == '\n') {
                    text += "\\n";
                } else if (ch == '\r') {
                    text += "\\r";
                } else if (static_cast<unsigned char>(ch) >= 0x20) {
                    text += ch;
                }
            }
            j += std::format(", \"chosen_edid\": \"{}\", \"chosen_text\": \"{}\"", screens->edid(*s), text);
        }
    }
    j += std::format(", \"eligible_count\": {}, \"conditioned\": {}, \"conditioned_at_setup\": {}, "
                     "\"rng_index_before\": {}, \"rng_index_after\": {}, \"eligible\": [",
                     l.choice().eligible.size(), l.choice().conditioned, l.conditioned_at_setup(), l.rng_before(),
                     l.rng_after());
    for (size_t i = 0; i < l.choice().eligible.size(); ++i) {
        j += std::format("{}\"{:08X}\"", i ? ", " : "", l.choice().eligible[i]->form_id);
    }
    j += "]";
    if (mist) {
        auto& mm = static_cast<MistMenu&>(*mist);
        BackdropScene s{};
        mm.scene(s);
        j += std::format(", \"mist\": {{\"model\": \"{}\", \"default_logo\": {}, \"turned\": {:.9g}, \"fov\": {:.9g}, "
                         "\"frame_seconds\": {:.9g}, \"path_seconds\": {:.9g}, \"camera\": [{:.9g}, {:.9g}, {:.9g}], "
                         "\"camera_rotation\": [",
                         s.model, s.default_logo, mm.turned(), mm.fov(), mm.frame_seconds(), mm.path_seconds(),
                         s.camera[0], s.camera[1], s.camera[2]);
        for (int i = 0; i < 9; ++i) {
            j += std::format("{}{:.9g}", i ? ", " : "", s.camera_rotation[i]);
        }
        j += "], \"model_rotation\": [";
        for (int i = 0; i < 9; ++i) {
            j += std::format("{}{:.9g}", i ? ", " : "", s.model_rotation[i]);
        }
        j += std::format("], \"model_translation\": [{:.9g}, {:.9g}, {:.9g}], \"model_scale\": {:.9g}}}",
                         s.model_translation[0], s.model_translation[1], s.model_translation[2], s.model_scale);
    }
    return j + "}";
}

} // namespace spud::ui
