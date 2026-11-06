#pragma once

// The Stats Menu's starfield: the skydome with its
// eighteen constellations, a star per perk and a line per perk link placed from the skills' perk trees, and
// the Stats Menu's own camera: its opening flight (CameraIntro), the ring turned by the menu's spin, the
// moves into a perk tree and back, and the constellation images that fade in when the camera rests on a
// skill. What the menu decides (the spin, the selection, the cards) stays in menus_stats.cpp; this is the
// scene it drives.

#include "ui/character.hpp"
#include "ui/scene.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <string>
#include <vector>

namespace spud::fx {
class EffectInstance;
}

namespace spud::ui {

class Ui;

class Starfield {
  public:
    explicit Starfield(Ui& ui);
    ~Starfield();

    // The skills on Point00..17 in the menu's order with their trees.
    void set_trees(std::vector<PerkTree> trees) { m_trees = std::move(trees); }
    // Stars and lines take the trees' new states (a perk taken or refunded).
    void update_states(std::vector<PerkTree> trees);
    // g_StatsMenuBeastMode: the vampire skydome, the two beast trees (WerewolfPerks on Point00,
    // VampirePerks on Point01), the ring axis from Point00 and Point02. Before the first update.
    void set_beast(bool beast) { m_beast = beast; }
    // Each frame: the game clock (ms), the selected skill, and the ring's direction the menu sends
    // (SetDirection).
    void update(double now_ms, int selected, double direction);
    // The menu's events: a spin input (the selected image fades out), the spin's stop (it fades in), the move
    // into the selected skill's tree and back to the ring.
    void spin_started();
    void spin_stopped();
    void enter_tree();
    void leave_tree();
    // The hide (StatsMenu::ProcessMessage message 3): CameraOutro, the camera following CameraPosition out.
    void close();
    // The selected perk's node in a tree, null on the ring.
    const PerkTree::Node* selected_node() const;
    // A stick direction in a tree: the next star, in `skill` or a neighbour's, which `skill`
    // then names. False when there is none that way.
    bool move_star(double x, double y, int& skill);
    // A click (StatsStarSelectorPC): the star nearest the point on the screen among the selected skill's and its
    // neighbours'; true when it's the selected one, within reach.
    bool click_star(float x, float y, int& skill);
    struct StarLabel {
        const PerkTree::Node* node;
        float x, y; // of the screen, y down
    };
    // Every star in front of the camera on the screen.
    void labels(std::vector<StarLabel>& out) const;
    bool built() const { return m_built; }
    bool opening() const { return m_state == state_opening; }
    // Slot 6's scene: the skydome shape by shape, the stars and lines once the trees are up, the Stats
    // camera.
    bool scene(UiScene& out) const;
    std::string state_json() const;

  private:
    enum State { state_opening = 2, state_ring = 0, state_tree = 1, state_closing = 3 };
    struct Star {
        int skill = 0;
        size_t node = 0;       // in m_trees[skill].nodes
        glm::vec3 local{0.0f}; // in the Point's frame
        glm::vec3 world{0.0f};
        glm::vec3 camera_point{0.0f}; // + fStatsStarCameraOffset, world
    };
    struct Line {
        size_t star = 0;          // the parent star
        size_t child = 0;         // which of its children
        glm::mat3 rotation{1.0f}; // under the star: the local rotation (not orthonormal) and uniform scale
        float scale = 1.0f;
        bool unlocked = false; // both ends owned
    };
    void build();
    glm::mat3 ring_pose(int skill) const;
    void start_move(const glm::mat3& rotation, const glm::vec3& position, double ms);
    void fade(int image, int state);
    float image_alpha(int image) const;
    void attach();
    void select_star(int star);
    bool project(const glm::vec3& point, float& x, float& y) const;
    int find_star(const glm::vec2& direction, int skill) const;
    void add_billboarded(UiScene& out, uint32_t model, const SceneModel& m, const fx::EffectInstance* anim,
                         const std::vector<int>& materials, const glm::mat3& root_rotation, float root_scale,
                         const glm::vec3& root_t, const glm::mat3& cam, float sort) const;
    float order_key(uint32_t dome_node, float distance_sq) const;

    Ui& m_ui;
    std::vector<PerkTree> m_trees;
    std::shared_ptr<const SceneModel> m_dome, m_star, m_line;
    std::unique_ptr<fx::EffectInstance> m_dome_anim; // the skydome's sequences (CameraIntro moves the camera)
    // The stars' and lines' sequences as their behaviour graphs play them (one instance each), attached with
    // the stars; and per model the effect material each .osm submesh takes from its instance (-1: none).
    std::vector<std::unique_ptr<fx::EffectInstance>> m_star_anims, m_line_anims;
    bool m_attached = false;
    double m_anim_now = 0;
    std::vector<int> m_dome_materials, m_star_materials, m_line_materials;
    bool m_built = false;
    bool m_beast = false;
    State m_state = state_opening;
    double m_now = 0, m_intro_start = 0;
    int m_selected = 15;
    glm::vec3 m_axis{0, 0, 1};       // menu
    std::vector<glm::vec3> m_points; // Point00.. world positions
    std::vector<glm::mat3> m_point_rot;
    std::vector<Star> m_stars;
    std::vector<Line> m_lines;
    // The camera (menu): pose, the ring's reference direction, a move in progress.
    glm::mat3 m_rotation{1.0f};
    glm::vec3 m_position{0.0f};
    glm::mat3 m_pose{1.0f};
    double m_pose_direction = 0;
    double m_direction = 0;
    bool m_moving = false;
    glm::quat m_q0{1, 0, 0, 0}, m_q1{1, 0, 0, 0};
    glm::vec3 m_p0{0.0f}, m_p1{0.0f};
    double m_move_start = 0, m_move_ms = 0;
    int m_perk_star = -1; // the selected perk's star in a tree
    // The images' fades (menu + 16 i: state 1 in, 2 out, and the start).
    struct Fade {
        int state = 0;
        double start = 0;
        float alpha = 0;
    };
    std::vector<Fade> m_fades;
    std::vector<int> m_order; // per skydome node: its OrderedRenderingNode child's index, -1 outside it
    int m_order_count = 0;
    std::vector<uint32_t> m_point_node; // Point k's node
    float m_aspect = 16.0f / 9.0f;
};

} // namespace spud::ui
