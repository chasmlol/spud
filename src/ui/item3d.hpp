#pragma once

// The item menus' and the Magic Menu's 3D item (g_Inventory3DManager
// section 2): the selected entry's model in UI 3D scene slot 1 (InventoryMenu, ContainerMenu, BarterMenu,
// GiftMenu, the crafting submenus) or 2 (MagicMenu), placed, zoomed and turned as the game does.
// The model of an entry (a weapon's first-person WNAM model, a book's INAM model, a spell's MDOB, an armour's
// world model for the player's sex, ...) is the game system's to name (ItemMenuData::models); this class does
// what the manager does with it.

#include "ui/scene.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

namespace spud::fx {
class EffectInstance;
}

namespace spud::ui {

class Ui;

class Item3D {
  public:
    explicit Item3D(Ui& ui);

    // Inventory3DManager::Begin for `slot`: the base position and scale (the Wide ones above
    // 4:3).
    void begin(int slot);
    // The Magic Menu after Begin(2): no rotation or zoom ( = 0), no bound normalisation ( = 0,
    // its constructor), and the game with fMagic3DItemPos*.
    void magic();
    // The game: show `model` (a cache.osm path; empty: an entry without
    // one, which leaves the shown model as it is). Clear3D: hide it.
    void show(const std::string& model);
    void clear();
    // The game: false when refused. zoom_direction / zoom_progress:
    // /.
    bool toggle_zoom();
    float zoom_direction() const { return m_zoom_dir; }
    float zoom_progress() const { return m_zoom; }
    bool interactive() const { return m_interactive; }
    // ProcessMouseMove while the movie's StartMouseRotation holds (g_ItemMenuMouseRotating): the
    // mouse's dx, dy through the game; ProcessThumbstick for the "Rotate"
    // stick.
    void mouse_move(float dx, float dy, float dt);
    void thumbstick(float x, float y, float dt);
    // The game: EulerZYX(a, 0, b) about the bound's centre, in scene axes.
    void rotate(float a, float b);
    // Each frame (Inventory3DManager::Render): a model whose scene graph has arrived is attached,
    // the zoom advances.
    void update(float dt);
    // The slot's scene (camera, lights, the shown model); false when nothing is shown.
    bool scene(UiScene& out) const;
    // Automation: the manager's state as JSON.
    std::string state_json() const;

    void set_mouse_settings(float sensitivity, float x_scale, float y_scale, bool invert_y) {
        m_mouse = {sensitivity, x_scale, y_scale, invert_y};
    }

  private:
    struct Entry {
        std::string model;
        std::shared_ptr<const SceneModel> graph;
        glm::mat3 rotation{1.0f}; // the root's local rotation (no scale)
        glm::vec3 translation{0.0f};
        float scale = 1.0f;
        float radius = 0.0f;    // the root's world-bound radius at attach (cache entry)
        glm::vec3 center{0.0f}; // the bound centre in the root's frame (no scale)
        float zoom = 1.0f;      // the INV marker's zoom
        bool hidden = false;    // NiAVObject flag bit 0 (Clear3D)
        // The model's particle systems (the spell art's), simulated by src/fx as the Mist Menu's fog is,
        // placed with the root; their drawing is the game's.
        std::shared_ptr<fx::EffectInstance> particles;
    };

    void attach(Entry& e);
    void place(Entry& e);

    Ui& m_ui;
    int m_slot = 0;
    glm::vec3 m_base{0.0f}, m_position{0.0f};
    float m_base_scale = 1.0f, m_scale = 1.0f;
    float m_zoom_dir = 0.0f, m_zoom = 0.0f;
    bool m_interactive = true;
    bool m_normalise = true;
    bool m_toggled = false;
    std::vector<Entry> m_cache;                // at most 7, the shown one last (count)
    std::string m_pending;                     // a model whose scene graph is still loading
    struct Mouse {
        float sensitivity = 0.0125f, x_scale = 0.02f, y_scale = 0.85f; // fMouseHeading*:Controls, compiled
        bool invert_y = false;                                         // bInvertYValues:Controls
    } m_mouse;
};

} // namespace spud::ui
