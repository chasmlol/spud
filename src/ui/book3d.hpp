#pragma once

// The Book Menu's book: the book's model (its INAM
// static's), its flight from where it starts to the reading pose and back
// (BookMenu::ProcessMessage), and its pages: the shapes "Base
// Page1".."Base Page4" show render targets 105..108, into which the Book
// movie is drawn each frame, its viewport from the hidden PageText shape's
// UVs.

#include "ui/scene.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <string>

namespace spud::ui {

class Ui;

class Book3D {
  public:
    explicit Book3D(Ui& ui);

    // The game: the model (cache.osm path), where the book starts (from the world:
    // the fixed point, the reference's world rotation and local scale) and whether it is a note.
    void open(const std::string& model, const float (&position)[3], const float (&rotation)[9], float scale);
    // Each update (the menu's flag 0x100: every frame): the model attached once its scene graph is in, the
    // flight.
    void update(double now_ms);
    // The first hide: the flight back (true while it runs; the menu goes when it has ended).
    bool close();
    bool closing_done() const { return m_closing && m_done; }

    bool ready() const { return m_graph != nullptr; }
    bool note() const { return m_note; }
    int pages() const { return m_pages; }
    // The Book movie's viewport on a page target of `size` pixels: left,
    // top, width, height.
    void viewport(uint32_t size, int (&out)[4]) const;
    // Base Page1's diffuse texture, which every page starts from (a cache path; "" when none).
    std::string paper() const;
    bool scene(UiScene& out) const;
    std::string state_json() const;

  private:
    void set_up_flight();
    glm::mat3 end_rotation() const;
    glm::vec3 bound_offset() const; // the root's world-bound centre less its translation (BookMenu flight)

    Ui& m_ui;
    std::string m_model;
    std::shared_ptr<const SceneModel> m_graph;
    glm::vec3 m_start_position{0.0f};
    glm::mat3 m_start_rotation{1.0f};
    float m_start_scale = 1.0f;
    // The root's local transform.
    glm::vec3 m_translation{0.0f};
    glm::mat3 m_rotation{1.0f};
    float m_scale = 1.0f;
    bool m_note = false;
    int m_pages = 0;
    int m_page_submesh[4] = {-1, -1, -1, -1};
    float m_uv[4] = {0, 1, 0, 1}; // u min, u max, v min, v max
    int m_counter = 0;            // the menu's update count
    // g_BookMenuFlight.
    glm::vec3 m_p0{0.0f}, m_p1{0.0f};
    glm::quat m_q0{1, 0, 0, 0}, m_q1{1, 0, 0, 0};
    float m_k0 = 0, m_k1 = 1, m_s0 = 1, m_s1 = 1;
    double m_duration = 1000, m_start = 0;
    bool m_done = false, m_closing = false;
    double m_now = 0;
};

} // namespace spud::ui
