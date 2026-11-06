#include "ui/book3d.hpp"

#include "ui/ui.hpp"

#include "formats/osm.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace spud::ui {

namespace {

// The settings the Book Menu reads (compiled values, equal to the live ones in the game: settings.csv of run
// stats6).
constexpr float k_width_percentage = 0.5f;    // fBookPosWidthPercentage:Interface
constexpr float k_height_percentage = 0.445f; // fBookPosHeightPercentage:Interface (0.444999993)
constexpr float k_book_distance = 110.0f;     // fBookDistance:Interface
constexpr float k_note_distance = 90.0f;      // fNoteDistance:Interface
constexpr double k_open_ms = 1000.0;          // fBookOpenTime:Interface
constexpr float k_x_rotation = -25.0f;        // fBookXRotation:Interface, degrees
constexpr const char* k_pages[4] = {"Base Page1", "Base Page2", "Base Page3", "Base Page4"};

void put(float (&out)[9], const glm::mat3& m) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r * 3 + c] = m[c][r];
        }
    }
}

// NiMatrix3's elementary rotations (as item3d.cpp): Rx = [1 0 0; 0 c s; 0 -s c], Ry = [c 0 -s; 0 1 0; s 0 c],
// Rz = [c s 0; -s c 0; 0 0 1], rows.
glm::mat3 rx(float t) {
    const float c = std::cos(t), s = std::sin(t);
    return glm::mat3(1, 0, 0, 0, c, -s, 0, s, c);
}
glm::mat3 ry(float t) {
    const float c = std::cos(t), s = std::sin(t);
    return glm::mat3(c, 0, s, 0, 1, 0, -s, 0, c);
}
glm::mat3 rz(float t) {
    const float c = std::cos(t), s = std::sin(t);
    return glm::mat3(c, -s, 0, s, c, 0, 0, 0, 1);
}

// The game: a normalised lerp with Blow's correction of the parameter.
glm::quat fast_slerp(const glm::quat& q0, const glm::quat& q1, float t) {
    const float d = q0.w * q1.w + q0.x * q1.x + q0.y * q1.y + q0.z * q1.z;
    const float k = 0.58549219f * (1.0f - 0.82279688f * d) * (1.0f - 0.82279688f * d);
    auto correct = [k](float u) { return u * (k * u * (2.0f * u - 3.0f) + 1.0f + k); };
    const float tc = t > 0.5f ? 1.0f - correct(1.0f - t) : correct(t);
    const glm::quat q(q0.w + (q1.w - q0.w) * tc, q0.x + (q1.x - q0.x) * tc, q0.y + (q1.y - q0.y) * tc,
                      q0.z + (q1.z - q0.z) * tc);
    return glm::normalize(q);
}

} // namespace

Book3D::Book3D(Ui& ui) : m_ui(ui) {}

void Book3D::open(const std::string& model, const float (&position)[3], const float (&rotation)[9],
                  float scale) {
    m_model = model;
    m_graph.reset();
    m_start_position = glm::vec3(position[0], position[1], position[2]);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            m_start_rotation[c][r] = rotation[r * 3 + c];
        }
    }
    m_start_scale = std::abs(scale);
    m_translation = m_start_position;
    m_rotation = m_start_rotation;
    m_scale = m_start_scale;
    m_counter = 0;
    m_done = false;
    m_closing = false;
}

void Book3D::update(double now_ms) {
    m_now = now_ms;
    if (!m_graph) {
        if (m_model.empty()) {
            return;
        }
        // once the model is in; then SetupPages finds the page shapes (fewer than four: a
        // note) and the PageText shape whose UV rectangle sizes the Book movie's viewport.
        m_graph = m_ui.scene_models().get(m_model);
        if (!m_graph) {
            return;
        }
        m_pages = 0;
        for (int i = 0; i < 4; ++i) {
            const uint32_t n = m_graph->find(k_pages[i]);
            const int sm = n == ose::k_none ? -1 : m_graph->submesh_of(n);
            if (n == ose::k_none) {
                break;
            }
            m_page_submesh[i] = sm;
            ++m_pages;
        }
        m_note = m_pages < 4;
        if (const uint32_t pt = m_graph->find("PageText");
            pt != ose::k_none && pt < m_graph->graph.node_extras.size()) {
            const ose::NodeExtra& x = m_graph->graph.node_extras[pt];
            if (x.kind == ose::NodeKind::shape) {
                std::copy(x.uv_rect, x.uv_rect + 4, m_uv);
            }
        }
    }
    // BookMenu::ProcessMessage, update: updates 0-4 leave the book where it starts; the fifth sets the flight
    // up; from then on the flight is evaluated, and a done flight leaves the pose as it was last written.
    if (m_counter < 5) {
        ++m_counter;
        if (m_counter == 5) {
            set_up_flight();
            m_counter = 6;
        }
        return;
    }
    if (m_done) {
        return;
    }
    if (m_now > m_start + m_duration) {
        m_done = true;
        return;
    }
    const double u = (m_now - m_start) / m_duration;
    const float t =
        std::clamp(float((1.0 - std::sin(3.14159265358979 * u + 1.5707963267948966)) * 0.5), 0.0f, 1.0f);
    const glm::vec3 offset = bound_offset(); // from the previous update's pose
    m_scale = std::abs(m_s0 + (m_s1 - m_s0) * t);
    m_rotation = glm::mat3_cast(fast_slerp(m_q0, m_q1, t));
    m_translation = m_p0 + (m_p1 - m_p0) * t;
    if (!m_note) {
        m_translation -= offset * (m_k0 + (m_k1 - m_k0) * t);
    }
}

glm::mat3 Book3D::end_rotation() const {
    // Rx(fBookXRotation) x EulerXYZ(the root's "INV" angles x 0.001); without an INV marker [-1 0 0; 0 0 1; 0
    // 1 0].
    if (m_graph && m_graph->has_inv()) {
        const ose::InvMarker& inv = m_graph->graph.inv_marker[0];
        const glm::mat3 m =
            rx(inv.rotation[0] * 0.001f) * ry(inv.rotation[1] * 0.001f) * rz(inv.rotation[2] * 0.001f);
        return rx(k_x_rotation * 0.017453292f) * m;
    }
    return glm::mat3(glm::vec3(-1, 0, 0), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0));
}

glm::vec3 Book3D::bound_offset() const {
    // The root's world-bound centre less its local translation, the slot root at identity.
    // The book's shapes are skinned: their bounds follow the bones, here
    // at the file's rest pose, the closed book; the game's bones are posed open by the book's Havok graph
    if (!m_graph) {
        return glm::vec3(0.0f);
    }
    glm::vec3 c;
    float r;
    m_graph->world_bound(m_graph->graph.root, m_rotation * m_scale, glm::vec3(0.0f), c, r);
    return r > 0.0f ? c : glm::vec3(0.0f);
}

void Book3D::set_up_flight() {
    // The target: a ray from the scene manager's camera through the pixel (trunc(0.5 W), trunc(0.445 H))
    // fBookDistance x W / H
    // along it (fNoteDistance for a note).
    const float w = float(m_ui.target_width()), h = float(std::max(m_ui.target_height(), 1));
    const SceneCamera cam = default_scene_camera(w, h);
    const float px = std::trunc(k_width_percentage * w), py = std::trunc(k_height_percentage * h);
    const float fl = cam.frustum[0], fr = cam.frustum[1], ft = cam.frustum[2], fb = cam.frustum[3];
    const float fx = (px / w) * (fr - fl) + fl;
    const float fy = (1.0f - py / h) * (ft - fb) + fb;
    glm::mat3 r;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            r[col][row] = cam.rotation[row * 3 + col];
        }
    }
    const glm::vec3 dir = glm::normalize(r[0] + fy * r[1] + fx * r[2]);
    const glm::vec3 origin(cam.position[0], cam.position[1], cam.position[2]);
    const float distance = (m_note ? k_note_distance : k_book_distance) * w / h;
    m_p0 = m_start_position;
    m_p1 = origin + dir * distance;
    m_q0 = glm::quat_cast(m_start_rotation);
    m_q1 = glm::quat_cast(end_rotation());
    if (glm::dot(m_q0, m_q1) < 0.0f) {
        m_q1 = -m_q1;
    }
    m_k0 = 0.0f;
    m_k1 = 1.0f;
    m_s0 = m_start_scale;
    m_s1 = 1.0f;
    m_duration = k_open_ms;
    m_start = std::floor(m_now);
    m_done = false;
}

bool Book3D::close() {
    // The first hide once the open flight is done: back to where it started, no bound term, the same ease.
    if (!m_done || m_closing) {
        return m_closing && !m_done;
    }
    m_closing = true;
    m_p0 = m_translation;
    m_p1 = m_start_position;
    m_q0 = glm::quat_cast(m_rotation);
    m_q1 = glm::quat_cast(m_start_rotation);
    if (glm::dot(m_q0, m_q1) < 0.0f) {
        m_q1 = -m_q1;
    }
    m_k0 = m_k1 = 0.0f;
    m_s0 = 1.0f;
    m_s1 = m_start_scale;
    m_duration = k_open_ms;
    m_start = std::floor(m_now);
    m_done = false;
    return true;
}

void Book3D::viewport(uint32_t size, int (&out)[4]) const {
    const float s = float(size);
    out[0] = int(std::trunc(m_uv[0] * s));
    out[1] = int(std::trunc(m_uv[2] * s));
    out[2] = int(std::trunc((m_uv[1] - m_uv[0]) * s));
    out[3] = int(std::trunc((m_uv[3] - m_uv[2]) * s));
}

std::string Book3D::paper() const {
    if (!m_graph || m_page_submesh[0] < 0 || size_t(m_page_submesh[0]) >= m_graph->submesh_textures.size()) {
        return {};
    }
    return m_graph->submesh_textures[size_t(m_page_submesh[0])][osm::texture_diffuse];
}

bool Book3D::scene(UiScene& out) const {
    if (!m_graph) {
        return false;
    }
    out.slot = 3;
    out.camera = default_scene_camera(float(m_ui.target_width()), float(m_ui.target_height()));
    // Slot 3's lights: fBookLight2* is created first, then fBookLight*.
    out.lights.push_back({{10.0f, -75.0f, 10.0f}, {0.98f, 0.95f, 0.83f}, 400.0f, 1.0f, false});
    out.lights.push_back({{100.0f, -350.0f, 100.0f}, {0.98f, 0.95f, 0.83f}, 400.0f, 1.75f, false});
    // The root's transform in place of the authored one baked into the .osm (version 1).
    const SceneModel& m = *m_graph;
    const uint32_t root = m.graph.root;
    glm::mat3 world_r = m_rotation * m_scale;
    glm::vec3 world_t = m_translation;
    if (root != ose::k_none && osm::k_version < 2) {
        const glm::mat3 inv = glm::inverse(m.rest_rotation[root]);
        world_r = world_r * inv;
        world_t = world_t - world_r * m.rest_translation[root];
    }
    const uint32_t model = out.model(m_model);
    for (size_t i = 0; i < m.graph.submesh_nodes.size(); ++i) {
        SceneDraw d;
        d.model = model;
        d.submesh = int(i);
        put(d.rotation, world_r);
        for (int k = 0; k < 3; ++k) {
            d.translation[k] = world_t[k];
        }
        for (int p = 0; p < m_pages; ++p) {
            if (m_page_submesh[p] == int(i)) {
                d.page = p; // render target 105 + p in the diffuse slot
            }
        }
        out.draws.push_back(d);
    }
    return true;
}

std::string Book3D::state_json() const {
    auto vec = [](const glm::vec3& v) { return std::format("[{:.9g}, {:.9g}, {:.9g}]", v.x, v.y, v.z); };
    std::string rot;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            rot += std::format("{}{:.9g}", r || c ? ", " : "", m_rotation[c][r]);
        }
    }
    auto quat = [](const glm::quat& q) {
        return std::format("[{:.9g}, {:.9g}, {:.9g}, {:.9g}]", q.w, q.x, q.y, q.z);
    };
    // The flight): ends, quaternions (w, x, y, z),
    // the bound-term weights, start and duration (ms), and the clock of the last update.
    const std::string flight = std::format(
        "{{\"p0\": {}, \"p1\": {}, \"q0\": {}, \"q1\": {}, \"k\": [{:.9g}, {:.9g}], \"s\": [{:.9g}, {:.9g}], "
        "\"duration\": {:.9g}, \"start\": {:.9g}, \"done\": {}}}",
        vec(m_p0), vec(m_p1), quat(m_q0), quat(m_q1), m_k0, m_k1, m_s0, m_s1, m_duration, m_start,
        int(m_done));
    return std::format(
        "{{\"model\": \"{}\", \"ready\": {}, \"pages\": {}, \"note\": {}, \"counter\": {}, \"done\": {}, "
        "\"closing\": {}, \"uv_rect\": [{:.9g}, {:.9g}, {:.9g}, {:.9g}], \"target\": {}, \"flight\": {}, "
        "\"clock_ms\": {:.9g}, \"root\": {{\"translate\": {}, \"rotate\": [{}], \"scale\": {:.9g}}}}}",
        m_model, m_graph != nullptr, m_pages, m_note, m_counter, m_done, m_closing, m_uv[0], m_uv[1], m_uv[2],
        m_uv[3], vec(m_p1), flight, m_now, vec(m_translation), rot, m_scale);
}

} // namespace spud::ui
