#include "ui/item3d.hpp"

#include "ui/ui.hpp"

#include "core/units.hpp"
#include "formats/osm.hpp"
#include "fx/effect.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace spud::ui {

namespace {

// The settings the manager reads (compiled values; this install's INIs leave them alone, measured in the
// game's settings list).
constexpr glm::vec3 k_pos{-38.0f, -500.0f, 16.0f};          // fInventory3DItemPosX/Y/Z:Interface
constexpr float k_pos_scale = 1.87f;                        // fInventory3DItemPosScale
constexpr glm::vec3 k_pos_wide{-29.0f, -500.0f, 12.0f};     // fInventory3DItemPosX/Y/ZWide
constexpr float k_pos_scale_wide = 1.5f;                    // fInventory3DItemPosScaleWide
constexpr float k_zoom_speed = 1.5f;                        // fInventory3DItemZoomSpeed
constexpr glm::vec3 k_zoom_pos{0.0f, -500.0f, 0.0f};        // fInventory3DItemZoomX/Y/Z
constexpr float k_zoom_scale = 2.25f;                       // fInventory3DItemZoomScale
constexpr float k_rot_speed = 3.0f;                         // fInventory3DItemRotSpeed
constexpr float k_rot_mouse_speed = 50.0f;                  // fInventory3DItemRotMouseSpeed
constexpr float k_bound_radius_scale = 12.5f;               // fInventory3DBoundRadiusScale
constexpr glm::vec3 k_magic_pos{34.7f, -500.0f, 8.0f};      // fMagic3DItemPosX/Y/Z
constexpr float k_magic_scale = 1.87f;                      // fMagic3DItemPosScale
constexpr glm::vec3 k_magic_pos_wide{26.0f, -500.0f, 6.0f}; // fMagic3DItemPosX/Y/ZWide
constexpr float k_magic_scale_wide = 1.75f;                 // fMagic3DItemPosScaleWide
constexpr size_t k_cache = 7;                               // The game keeps 7 models

// NiMatrix3's elementary rotations (): Rx = [1 0 0; 0 c s; 0 -s c], Ry = [c 0 -s; 0 1 0;
// s 0 c], Rz = [c s 0; -s c 0; 0 0 1], rows; glm is column-major, so each is written transposed.
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

float aspect(const Ui& ui) {
    return float(ui.target_width()) / float(std::max(ui.target_height(), 1));
}

// A node or any of its ancestors named `name`.
bool under(const SceneModel& m, uint32_t node, std::string_view name) {
    for (uint32_t n = node; n != ose::k_none; n = m.graph.nodes[n].parent) {
        if (m.graph.string(m.graph.nodes[n].name) == name) {
            return true;
        }
    }
    return false;
}

} // namespace

Item3D::Item3D(Ui& ui) : m_ui(ui) {}

void Item3D::begin(int slot) {
    m_slot = slot;
    const bool wide = aspect(m_ui) > 1.3333334f;
    m_base = m_position = wide ? k_pos_wide : k_pos;
    m_base_scale = m_scale = wide ? k_pos_scale_wide : k_pos_scale;
    m_zoom_dir = m_zoom = 0.0f;
    m_interactive = true;
    m_toggled = false;
    m_cache.clear();
    m_pending.clear();
}

void Item3D::magic() {
    m_interactive = false;
    m_normalise = false;
    const bool wide = aspect(m_ui) > 1.3333334f;
    m_base = m_position = wide ? k_magic_pos_wide : k_magic_pos;
    m_base_scale = m_scale = wide ? k_magic_scale_wide : k_magic_scale;
}

void Item3D::show(const std::string& model) {
    if (model.empty()) {
        return; // no model form: the shown model stays ( does nothing)
    }
    if (!m_pending.empty() && m_pending == model) {
        return;
    }
    for (size_t i = 0; i < m_cache.size(); ++i) {
        if (m_cache[i].model != model) {
            continue;
        }
        if (i + 1 == m_cache.size() && !m_cache[i].hidden) {
            return; // already shown
        }
        // A cached model: the shown one hidden, this one shown and swapped to the end, keeping its placement.
        clear();
        m_cache[i].hidden = false;
        std::swap(m_cache[i], m_cache.back());
        return;
    }
    clear();
    m_pending = model;
    update(0.0f); // resident: attached in the same call
}

void Item3D::clear() {
    if (!m_pending.empty()) {
        m_pending.clear(); // the pending load is cancelled
        return;
    }
    if (!m_cache.empty()) {
        m_cache.back().hidden = true;
    }
}

void Item3D::attach(Entry& e) {
    // The game: the rotation set from the "INV" marker's Euler angles (x
    // 0.001) or the identity, the world-bound radius after an update with the NIF's own root scale, then
    // PlaceModel.
    const SceneModel& m = *e.graph;
    if (m.has_inv()) {
        const ose::InvMarker& inv = m.graph.inv_marker[0];
        e.rotation =
            rx(inv.rotation[0] * 0.001f) * ry(inv.rotation[1] * 0.001f) * rz(inv.rotation[2] * 0.001f);
        e.zoom = inv.zoom;
    }
    const uint32_t root = m.graph.root;
    const float root_scale = root != ose::k_none ? m.graph.nodes[root].scale : 1.0f;
    // A weapon's scabbard is detached before that update: not in the bound.
    uint32_t scabbard = ose::k_none;
    for (uint32_t i = 0; i < m.graph.nodes.size() && scabbard == ose::k_none; ++i) {
        if (m.graph.string(m.graph.nodes[i].name) == "Scb") {
            scabbard = i;
        }
    }
    glm::vec3 c;
    float r;
    m.world_bound(root, glm::mat3(1.0f), glm::vec3(0.0f), c, r, scabbard);
    e.center = c;
    e.radius = r * root_scale;
    place(e);
}

void Item3D::place(Entry& e) {
    // The game: s = (12.5 / R with bound normalisation) x current scale x
    // INV zoom x (16/9) / aspect above 16:9; then the bound's centre on the current position (with
    // normalisation).
    float s = (m_normalise && e.radius > 0.0f ? k_bound_radius_scale / e.radius : 1.0f) * m_scale * e.zoom;
    const float a = aspect(m_ui);
    if (a > 1.7777778f) {
        s *= 1.7777778f / a;
    }
    e.scale = std::abs(s);
    const glm::vec3 d = m_normalise ? e.rotation * (e.scale * e.center) : glm::vec3(0.0f);
    e.translation = m_position - d;
}

bool Item3D::toggle_zoom() {
    if (!m_interactive || m_toggled || m_cache.empty() || !m_pending.empty() || m_cache.back().hidden) {
        return false;
    }
    if (m_zoom == 0.0f) {
        m_zoom_dir = 1.0f;
    } else if (m_zoom == 1.0f) {
        m_zoom_dir = -1.0f;
    } else {
        return false;
    }
    m_toggled = true;
    return true;
}

void Item3D::rotate(float a, float b) {
    if (m_cache.empty() || !m_pending.empty() || m_cache.back().hidden) {
        return;
    }
    // About the bound's centre: new rotation R x old, translation + d - R d with d the centre's offset.
    Entry& e = m_cache.back();
    const glm::mat3 r = rz(a) * ry(0.0f) * rx(b);
    const glm::vec3 d = e.rotation * (e.scale * e.center);
    e.rotation = r * e.rotation;
    e.translation = e.translation + d - r * d;
}

void Item3D::mouse_move(float dx, float dy, float dt) {
    if (!m_interactive) {
        return;
    }
    // ProcessMouseMove: (-dx k, dy k), k = fInventory3DItemRotMouseSpeed x dt, through ProcessLookInput's
    // mouse branch: y negated when inverted; x times sensitivity x X scale / dt, y times sensitivity x Y
    // scale; nothing without a frame time.
    if (dt <= 0.0f) {
        return;
    }
    const float k = k_rot_mouse_speed * dt;
    float a = -dx * k, b = dy * k;
    if (m_mouse.invert_y) {
        b = -b;
    }
    a = a * m_mouse.sensitivity * m_mouse.x_scale / dt;
    b = b * m_mouse.sensitivity * m_mouse.y_scale;
    rotate(a, b);
}

void Item3D::thumbstick(float x, float y, float dt) {
    if (!m_interactive) {
        return;
    }
    // ProcessThumbstick: the dominant axis only, at fInventory3DItemRotSpeed rad/s at full deflection.
    const float k = k_rot_speed * dt;
    if (std::abs(x) > std::abs(y)) {
        rotate(-x * k, 0.0f);
    } else {
        rotate(0.0f, -y * k);
    }
}

void Item3D::update(float dt) {
    if (!m_pending.empty()) {
        if (auto graph = m_ui.scene_models().get(m_pending)) {
            Entry e;
            e.model = m_pending;
            e.graph = std::move(graph);
            attach(e);
            if (!e.graph->graph.systems.empty()) {
                e.particles = std::make_shared<fx::EffectInstance>(fx::make_effect(e.graph->graph));
            }
            if (m_cache.size() >= k_cache) {
                m_cache.erase(m_cache.begin()); // the oldest
            }
            m_cache.push_back(std::move(e));
            m_pending.clear();
        } else if (m_ui.scene_models().failed(m_pending)) {
            m_pending.clear();
        }
    }
    // The shown model's particles advance with the frame, emitted from where the model is.
    if (!m_cache.empty() && m_cache.back().particles && !m_cache.back().hidden) {
        Entry& e = m_cache.back();
        e.particles->set_root_transform(e.rotation, units::to_metres(1.0f) * e.translation, e.scale);
        e.particles->advance(dt);
    }
    // The game: linear in progress, which stops at 0 or 1.
    if (m_zoom_dir == 0.0f) {
        return;
    }
    m_zoom += k_zoom_speed * dt * m_zoom_dir;
    if (m_zoom >= 1.0f || m_zoom <= 0.0f) {
        m_zoom = std::clamp(m_zoom, 0.0f, 1.0f);
        m_zoom_dir = 0.0f;
    }
    m_position = m_base + (k_zoom_pos - m_base) * m_zoom;
    m_scale = m_base_scale + (k_zoom_scale - m_base_scale) * m_zoom;
    if (!m_cache.empty()) {
        place(m_cache.back());
    }
    m_toggled = false;
}

bool Item3D::scene(UiScene& out) const {
    if (m_slot == 0 || m_cache.empty() || !m_pending.empty() || m_cache.back().hidden) {
        return false;
    }
    const Entry& e = m_cache.back();
    out.slot = m_slot;
    out.camera = default_scene_camera(float(m_ui.target_width()), float(m_ui.target_height()));
    if (m_slot == 1) {
        // Slot 1's lights: fInventoryLight* and fInventoryLight2*
        // (radius 0).
        out.lights.push_back({{100.0f, -350.0f, 100.0f}, {0.98f, 0.95f, 0.83f}, 400.0f, 1.75f, false});
        out.lights.push_back({{100.0f, -350.0f, 100.0f}, {0.98f, 0.95f, 0.83f}, 0.0f, 1.75f, false});
    }
    // The root's new transform in place of the authored one, which a version 1 .osm has baked in (from version 2
    // the importer leaves the root's own transform out, as the game does: book3d.cpp, starfield.cpp).
    const SceneModel& m = *e.graph;
    const uint32_t root = m.graph.root;
    const bool baked = root != ose::k_none && osm::k_version < 2;
    const glm::mat3 rest_r = baked ? m.rest_rotation[root] : glm::mat3(1.0f);
    const glm::vec3 rest_t = baked ? m.rest_translation[root] : glm::vec3(0.0f);
    const glm::mat3 inv_r = glm::inverse(rest_r);
    const glm::mat3 world_r = e.rotation * e.scale * inv_r;
    const glm::vec3 world_t = e.translation - world_r * rest_t;
    SceneDraw d;
    d.model = out.model(e.model);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            d.rotation[r * 3 + c] = world_r[c][r];
        }
        d.translation[r] = world_t[r];
    }
    // A weapon's scabbard ("Scb") is detached: its shapes are left out.
    bool scabbard = false;
    for (uint32_t n : m.graph.submesh_nodes) {
        scabbard = scabbard || (n != ose::k_none && under(m, n, "Scb"));
    }
    if (!scabbard) {
        out.draws.push_back(d);
        return true;
    }
    for (size_t i = 0; i < m.graph.submesh_nodes.size(); ++i) {
        const uint32_t n = m.graph.submesh_nodes[i];
        if (n != ose::k_none && under(m, n, "Scb")) {
            continue;
        }
        d.submesh = int(i);
        out.draws.push_back(d);
    }
    return true;
}

std::string Item3D::state_json() const {
    auto vec = [](const glm::vec3& v) { return std::format("[{:.9g}, {:.9g}, {:.9g}]", v.x, v.y, v.z); };
    std::string j =
        std::format("{{\"slot\": {}, \"base\": {}, \"base_scale\": {:.9g}, \"position\": {}, \"scale\": "
                    "{:.9g}, \"zoom_direction\": {:.9g}, \"zoom\": {:.9g}, \"interactive\": {}, "
                    "\"normalise\": {}, \"pending\": \"{}\", \"entries\": [",
                    m_slot, vec(m_base), m_base_scale, vec(m_position), m_scale, m_zoom_dir, m_zoom,
                    m_interactive, m_normalise, m_pending);
    for (size_t i = 0; i < m_cache.size(); ++i) {
        const Entry& e = m_cache[i];
        // The root's local rotate as the game holds it: row-major NiMatrix3.
        std::string rot;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                rot += std::format("{}{:.9g}", r || c ? ", " : "", e.rotation[c][r]);
            }
        }
        size_t particles = 0;
        for (size_t k = 0; e.particles && k < e.particles->system_count(); ++k) {
            particles += e.particles->particle_count(k);
        }
        j += std::format(
            "{}{{\"model\": \"{}\", \"hidden\": {}, \"rotate\": [{}], \"translate\": {}, \"scale\": {:.9g}, "
            "\"radius\": {:.9g}, \"center\": {}, \"inv_zoom\": {:.9g}, \"systems\": {}, \"particles\": {}}}",
            i ? ", " : "", e.model, e.hidden, rot, vec(e.translation), e.scale, e.radius, vec(e.center),
            e.zoom, e.particles ? e.particles->system_count() : 0, particles);
    }
    return j + "]}";
}

} // namespace spud::ui
