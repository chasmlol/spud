#include "ui/starfield.hpp"

#include "ui/ui.hpp"

#include "core/log.hpp"
#include "core/units.hpp"
#include "formats/osm.hpp"
#include "fx/effect.hpp"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace spud::ui {

namespace {

// The models (StatsMenu::StatsMenu, table).
constexpr const char* k_skydome = "meshes/interface/intperkskydome.osm";
constexpr const char* k_beast_skydome =
    "meshes/dlc01/interface/intvampireperkskydome.osm"; // the table's beast entry
constexpr const char* k_star = "meshes/interface/intperkstars01.osm";
constexpr const char* k_line = "meshes/interface/intperkline01.osm";
// The GMSTs the game and the camera read; compiled values, the live ones in
// the game equal them.
constexpr float k_star_x = 5.0f;                      // fStatsStarXIncrement
constexpr float k_star_y = 15.0f;                     // fStatsStarYIncrement
constexpr float k_star_z = -9.0f;                     // fStatsStarZIncrement
constexpr float k_star_z0 = 5.0f;                     // fStatsStarZInitialOffset
constexpr glm::vec3 k_star_camera{0.0f, -8.0f, 0.0f}; // fStatsStarCameraOffsetX/Y/Z
constexpr glm::vec3 k_star_look{0.0f, 0.0f, 1.0f};    // fStatsStarLookAtX/Y/Z
constexpr glm::vec3 k_skills_look{0.0f, 0.0f, 5.0f};  // fStatsSkillsLookAtX/Y/Z
constexpr float k_star_scale = 0.15f;                 // fStatsStarScale
constexpr float k_line_scale = 0.1175f;               // fStatsLineScale
constexpr float k_camera_fov = 50.0f;                 // fStatsCameraFOV
constexpr float k_camera_near = 1.0f;                 // fStatsCameraNearDistance
constexpr double k_fade_ms = 500.0;   // the images' fade (StatsMenu::ProcessMessage)
constexpr double k_settle_ms = 300.0; // the move after the intro
constexpr double k_tree_ms = 1000.0;  // moves into, inside and out of a tree

// The star's and the line's behaviour graphs (StarBehavior.hkx, Line.hkx): the sequence each state plays
constexpr const char* k_star_sequence[3] = {"Locked", "Unlocked", "Owned"};
constexpr const char* k_line_sequence[2] = {"Locked", "Unlocked"};

// A row-major NiMatrix3 <-> glm (column-major).
void put(float (&out)[9], const glm::mat3& m) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r * 3 + c] = m[c][r];
        }
    }
}

struct Affine {
    glm::mat3 r{1.0f}; // rotation x scale
    glm::vec3 t{0.0f};
    Affine operator*(const Affine& o) const { return {r * o.r, r * o.t + t}; }
    Affine inverse() const {
        const glm::mat3 i = glm::inverse(r);
        return {i, -(i * t)};
    }
};

// The draw transform of a model whose .osm holds its root's own transform baked in (.osm version 1, the
// importer composes the root like any node): the root's new world transform in place of the authored one.
Affine model_draw(const SceneModel& m, const Affine& root_world) {
    const uint32_t root = m.graph.root;
    if (root == ose::k_none || osm::k_version >= 2) {
        return root_world;
    }
    return root_world * Affine{m.rest_rotation[root], m.rest_translation[root]}.inverse();
}

Affine rest(const SceneModel& m, uint32_t node) {
    return {m.rest_rotation[node], m.rest_translation[node]};
}

// A node's transform in an effect instance's space after its last update (its sequences applied).
Affine animated(const fx::EffectInstance& a, uint32_t node) {
    return {a.node_world_rotation(node) * a.node_world_scale(node), a.node_world_translation_units(node)};
}

// Per .osm submesh, the instance's material that its controllers animate (-1: none).
std::vector<int> material_map(const fx::EffectInstance& a, size_t submeshes) {
    std::vector<int> map(submeshes, -1);
    for (size_t k = 0; k < a.material_count(); ++k) {
        const fx::MaterialState& s = a.material(k);
        if (s.source && s.source->owner_kind == ose::MaterialOwner::mesh_shape &&
            s.source->owner < submeshes) {
            map[s.source->owner] = int(k);
        }
    }
    return map;
}

void put_material(SceneDraw& d, const fx::MaterialState& s) {
    d.effect = true;
    for (int c = 0; c < 3; ++c) {
        d.base_color[c] = s.base_color[c] * s.base_color_scale;
    }
    d.base_color[3] = s.base_color[3];
    d.uv_offset[0] = s.uv_offset.x;
    d.uv_offset[1] = s.uv_offset.y;
    d.uv_scale[0] = s.uv_scale.x;
    d.uv_scale[1] = s.uv_scale.y;
}

// The game(2^32 - 1) x 2^-32 stands in as a fixed sequence: only the loops' phases depend on it.
float next_random(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) * (1.0f / 16777216.0f);
}

float scale_of(const glm::mat3& m) {
    return std::cbrt(std::abs(glm::determinant(m)));
}

glm::mat3 unit(const glm::mat3& m) {
    return glm::mat3(glm::normalize(m[0]), glm::normalize(m[1]), glm::normalize(m[2]));
}

// The game (mode & 7): the node's world rotation facing the camera.
// `w` the un-billboarded world rotation (unit), `p` its world position, camera at `c` with view, up, right
// axes.
glm::mat3 billboard(int mode, const glm::mat3& w, const glm::vec3& p, float s, const glm::vec3& c,
                    const glm::mat3& cam) {
    const glm::vec3 d = cam[0], u = cam[1], rt = cam[2];
    switch (mode & 7) {
    case 0: {
        const glm::vec3 z = -d;
        glm::vec3 y = w[1] - glm::dot(w[1], d) * d;
        const float len = glm::length(y);
        if (len <= 1e-6f) {
            return glm::mat3(-rt, -u, -d);
        }
        y /= len;
        return glm::mat3(glm::cross(y, z), y, z);
    }
    case 1: {
        const glm::vec3 q = glm::transpose(w) * (c - p) / std::max(s, 1e-12f);
        const float len = std::sqrt(q.x * q.x + q.z * q.z);
        if (len < 1e-12f) {
            return w;
        }
        const float sn = q.x / len, cs = q.z / len;
        // About local Y, taking local Z to (sn, 0, cs).
        const glm::mat3 ry(glm::vec3(cs, 0, -sn), glm::vec3(0, 1, 0), glm::vec3(sn, 0, cs));
        return w * ry;
    }
    case 2:
        return glm::mat3(rt, u, -d);
    default:
        return w;
    }
}

} // namespace

Starfield::Starfield(Ui& ui) : m_ui(ui) {}
Starfield::~Starfield() = default;

void Starfield::build() {
    // The game once the three models are in, then the trees (one an
    // update; here at once: they stay hidden until the opening has played, as the stars attach only in states
    // 0 and 1).
    const SceneModel& dome = *m_dome;
    const size_t n = m_trees.empty() ? 18 : m_trees.size();
    m_points.assign(n, glm::vec3(0.0f));
    m_point_rot.assign(n, glm::mat3(1.0f));
    // The skydome's root at the scene's origin (its local translation zeroed), its own rotation kept.
    const Affine root{dome.rest_rotation[dome.graph.root] *
                          (1.0f / std::max(dome.rest_scale[dome.graph.root], 1e-12f)) *
                          dome.rest_scale[dome.graph.root],
                      glm::vec3(0.0f)};
    const Affine draw = model_draw(dome, root);
    // BSOrderedNode "OrderedRenderingNode" draws its children in their order.
    m_order.assign(dome.graph.nodes.size(), -1);
    const uint32_t ordered = dome.find("OrderedRenderingNode");
    if (ordered != ose::k_none) {
        const std::vector<uint32_t> kids = dome.children(ordered);
        m_order_count = int(kids.size());
        for (size_t j = 0; j < kids.size(); ++j) {
            m_order[kids[j]] = int(j);
        }
        for (uint32_t i = 0; i < dome.graph.nodes.size(); ++i) {
            for (uint32_t a = i; a != ose::k_none && m_order[i] < 0; a = dome.graph.nodes[a].parent) {
                if (m_order[a] >= 0 && dome.graph.nodes[a].parent == ordered) {
                    m_order[i] = m_order[a];
                }
            }
        }
    }
    m_point_node.assign(n, ose::k_none);
    for (size_t k = 0; k < n; ++k) {
        const uint32_t node = dome.find(std::format("Point{:02d}", k));
        if (node == ose::k_none) {
            continue;
        }
        m_point_node[k] = node;
        const Affine w = draw * rest(dome, node);
        m_points[k] = w.t;
        m_point_rot[k] = w.r;
    }
    // The game: the ring axis from Point00 and Point01 (Point02 in beast mode,
    // whose Point01 is opposite Point00), negated.
    if (n >= 2) {
        const uint32_t p0 = dome.find("Point00");
        const uint32_t p1 = dome.find(m_beast ? "Point02" : "Point01");
        if (p0 != ose::k_none && p1 != ose::k_none) {
            const glm::vec3 a = (draw * rest(dome, p0)).t, b = (draw * rest(dome, p1)).t;
            m_axis = -glm::normalize(glm::cross(glm::normalize(a), glm::normalize(b)));
        }
    }
    // Stars, camera points and lines.
    m_stars.clear();
    m_lines.clear();
    for (size_t k = 0; k < m_trees.size() && k < n; ++k) {
        const PerkTree& t = m_trees[k];
        std::vector<int> star_of(t.nodes.size(), -1);
        for (size_t i = 0; i < t.nodes.size(); ++i) {
            const PerkTree::Node& nd = t.nodes[i];
            if (nd.perk == 0) {
                continue; // the root: no perk, no star
            }
            const float v = nd.y + nd.v;
            Star s;
            s.skill = int(k);
            s.node = i;
            s.local = glm::vec3((float(t.max_grid_x) * 0.5f - (nd.x + nd.h)) * k_star_x, v * k_star_y,
                                k_star_z0 - v * k_star_z);
            s.world = m_point_rot[k] * s.local + m_points[k];
            s.camera_point = m_point_rot[k] * (s.local + k_star_camera) + m_points[k];
            star_of[i] = int(m_stars.size());
            m_stars.push_back(s);
        }
        for (size_t i = 0; i < t.nodes.size(); ++i) {
            if (star_of[i] < 0) {
                continue;
            }
            const glm::vec3 p = m_stars[size_t(star_of[i])].local;
            for (uint32_t child : t.nodes[i].children) {
                const auto it = std::find_if(t.nodes.begin(), t.nodes.end(),
                                             [&](const PerkTree::Node& c) { return c.index == child; });
                if (it == t.nodes.end() || star_of[size_t(it - t.nodes.begin())] < 0) {
                    continue;
                }
                const glm::vec3 d = m_stars[size_t(star_of[size_t(it - t.nodes.begin())])].local - p;
                const float len = glm::length(d);
                if (len <= 0.0f) {
                    continue;
                }
                const glm::vec3 u = d / len;
                const glm::vec3 w =
                    glm::cross(m_axis, u); // the world axis used in the Point's frame, as stored
                const float s = std::abs(len * k_line_scale);
                Line l;
                l.star = size_t(star_of[i]);
                l.child = size_t(&child - t.nodes[i].children.data());
                // Rotation columns (u x w) / s, u, w / s, and the uniform scale s.
                l.rotation = glm::mat3(glm::cross(u, w) / s, u, w / s);
                l.scale = s;
                // The game: both perks owned.
                l.unlocked = t.nodes[i].state == 2 && it->state == 2;
                m_lines.push_back(l);
            }
        }
    }
    m_fades.assign(n, Fade{});
    // The skydome's sequences: CameraIntro at the build, which moves CameraPosition (the camera follows it).
    m_dome_anim = std::make_unique<fx::EffectInstance>(fx::make_effect(dome.graph));
    m_dome_materials = material_map(*m_dome_anim, dome.graph.submesh_nodes.size());
    m_intro_start = m_now;
    m_dome_anim->activate_sequence("CameraIntro", 1.0f, 0.0f, 0);
    m_dome_anim->update(0.0f);
    m_state = state_opening;
    m_pose = ring_pose(m_selected);
    m_pose_direction = m_direction;
    m_rotation = m_pose;
    m_position = glm::vec3(0.0f);
    m_built = true;
}

void Starfield::attach() {
    // StatsMenu::ProcessMessage, the first Update in state 0 or 1 with the graphs loaded: each
    // star's graph updated by 0 s and then by a random time in [0, 1) s in its start state (Locked), the
    // initial event (owned: OwnedWild, available: UnlockedWild; a line with both perks owned: Unlocked), then
    // the star on its Point.
    const auto star = fx::make_effect(m_star->graph);
    const auto line = fx::make_effect(m_line->graph);
    uint32_t seed = 1;
    m_star_anims.clear();
    m_line_anims.clear();
    for (const Star& s : m_stars) {
        auto a = std::make_unique<fx::EffectInstance>(star);
        a->activate_sequence(k_star_sequence[0]);
        a->update(0.0f);
        a->update(next_random(seed));
        const int state = std::clamp(m_trees[size_t(s.skill)].nodes[s.node].state, 0, 2);
        if (state != 0) {
            a->deactivate_sequence(k_star_sequence[0]);
            a->activate_sequence(k_star_sequence[state]); // the wildcard's ZeroDuration transition
            a->advance(0.0f);
        }
        m_star_anims.push_back(std::move(a));
    }
    for (const Line& l : m_lines) {
        auto a = std::make_unique<fx::EffectInstance>(line);
        a->activate_sequence(k_line_sequence[0]);
        a->update(0.0f);
        a->update(next_random(seed));
        if (l.unlocked) {
            a->deactivate_sequence(k_line_sequence[0]);
            a->activate_sequence(k_line_sequence[1]);
            a->advance(0.0f);
        }
        m_line_anims.push_back(std::move(a));
    }
    if (!m_star_anims.empty()) {
        m_star_materials = material_map(*m_star_anims[0], m_star->graph.submesh_nodes.size());
    }
    if (!m_line_anims.empty()) {
        m_line_materials = material_map(*m_line_anims[0], m_line->graph.submesh_nodes.size());
    }
    m_attached = true;
    m_anim_now = m_now;
}

glm::mat3 Starfield::ring_pose(int skill) const {
    // Looking from the origin at the Point plus fStatsSkillsLookAt: right = f x axis, up = right x f; the
    // camera's columns are view, up, right.
    if (skill < 0 || size_t(skill) >= m_points.size()) {
        return glm::mat3(1.0f);
    }
    const glm::vec3 f = glm::normalize(m_points[size_t(skill)] + k_skills_look);
    const glm::vec3 r = glm::normalize(glm::cross(f, m_axis));
    const glm::vec3 u = glm::normalize(glm::cross(r, f));
    return glm::mat3(f, u, r);
}

void Starfield::start_move(const glm::mat3& rotation, const glm::vec3& position, double ms) {
    // The game: from the camera's world rotation and position, the shorter arc.
    m_q0 = glm::normalize(glm::quat_cast(unit(m_rotation)));
    m_q1 = glm::normalize(glm::quat_cast(unit(rotation)));
    if (glm::dot(m_q0, m_q1) < 0.0f) {
        m_q1 = -m_q1;
    }
    m_p0 = m_position;
    m_p1 = position;
    m_move_start = m_now;
    m_move_ms = ms;
    m_moving = true;
}

void Starfield::fade(int image, int state) {
    if (image < 0 || size_t(image) >= m_fades.size()) {
        return;
    }
    Fade& f = m_fades[size_t(image)];
    if (state == 2 && f.state == 0 && f.alpha <= 0.0f) {
        return; // fading out only a lit or fading image
    }
    f.state = state;
    f.start = m_now;
}

float Starfield::image_alpha(int image) const {
    return image >= 0 && size_t(image) < m_fades.size() ? m_fades[size_t(image)].alpha : 0.0f;
}

void Starfield::spin_started() {
    if (m_state == state_ring) {
        fade(m_selected, 2);
    }
}

void Starfield::spin_stopped() {
    if (m_state == state_ring) {
        fade(m_selected, 1);
    }
}

void Starfield::enter_tree() {
    if (!m_built || m_state != state_ring) {
        return;
    }
    // up on the ring: the image fades out, the star nearest the camera of the
    // selected skill becomes the perk, and the camera moves to its camera point, looking 1 unit above the
    // star.
    fade(m_selected, 2);
    m_state = state_tree;
    int best = -1;
    float best_d = 0.0f;
    for (size_t i = 0; i < m_stars.size(); ++i) {
        if (m_stars[i].skill != m_selected) {
            continue;
        }
        const float d = glm::length(m_stars[i].world - m_position);
        if (best < 0 || d < best_d) {
            best = int(i);
            best_d = d;
        }
    }
    m_perk_star = best;
    if (best >= 0) {
        select_star(best);
    }
}

void Starfield::select_star(int star) {
    m_perk_star = star;
    const Star& s = m_stars[size_t(star)];
    const glm::vec3 d = glm::normalize(s.camera_point - (s.world + k_star_look));
    const glm::vec3 right = glm::normalize(glm::cross(m_axis, d));
    const glm::vec3 up = glm::normalize(glm::cross(d, right));
    start_move(glm::mat3(-d, up, right), s.camera_point, k_tree_ms);
}

const PerkTree::Node* Starfield::selected_node() const {
    if (m_perk_star < 0 || size_t(m_perk_star) >= m_stars.size()) {
        return nullptr;
    }
    const Star& s = m_stars[size_t(m_perk_star)];
    return &m_trees[size_t(s.skill)].nodes[s.node];
}

int Starfield::find_star(const glm::vec2& stick, int skill) const {
    // on the plane across the view, within 45 degrees of the stick. Under 25 degrees
    // beats over; among those a star linked to the selected one, then the nearest; over 25 the smallest angle.
    const glm::mat3 cam = unit(m_rotation);
    const glm::vec3 f = cam[0], up = cam[1], right = cam[2];
    const glm::vec3 g = glm::normalize(stick.x * right + stick.y * up);
    const Star& current = m_stars[size_t(m_perk_star)];
    const auto plane = [&f](const glm::vec3& p) { return p - glm::dot(p, f) * f; };
    const PerkTree::Node& node = m_trees[size_t(current.skill)].nodes[current.node];
    int best = -1;
    bool best_close = false, best_linked = false;
    float best_distance = 0, best_angle = 0;
    for (size_t i = 0; i < m_stars.size(); ++i) {
        const Star& c = m_stars[i];
        if (c.skill != skill || int(i) == m_perk_star) {
            continue;
        }
        const glm::vec3 v = plane(c.world) - plane(current.world);
        const float distance = glm::length(v);
        if (distance < 1e-5f) {
            continue;
        }
        const float angle = std::acos(std::clamp(glm::dot(v / distance, g), -1.0f, 1.0f)) * 57.29578f;
        if (angle >= 45.0f) {
            continue;
        }
        const PerkTree::Node& other = m_trees[size_t(c.skill)].nodes[c.node];
        const bool linked = c.skill == current.skill &&
                            (std::find(node.children.begin(), node.children.end(), other.index) != node.children.end() ||
                             std::find(other.children.begin(), other.children.end(), node.index) != other.children.end());
        const bool close = angle < 25.0f;
        bool better = best < 0;
        if (!better && close != best_close) {
            better = close;
        } else if (!better && close) {
            better = linked != best_linked ? linked : distance < best_distance;
        } else if (!better) {
            better = angle < best_angle;
        }
        if (better) {
            best = int(i);
            best_close = close;
            best_linked = linked;
            best_distance = distance;
            best_angle = angle;
        }
    }
    return best;
}

bool Starfield::move_star(double x, double y, int& skill) {
    if (!m_built || m_state != state_tree || m_perk_star < 0) {
        return false;
    }
    int star = find_star(glm::vec2(x, y), skill);
    if (star < 0 && !m_beast && x != 0.0) {
        const int count = int(m_trees.size());
        const int next = ((skill + (x > 0.0 ? 1 : -1)) % count + count) % count;
        star = find_star(glm::vec2(x, y), next);
        if (star >= 0) {
            skill = next;
        }
    }
    if (star < 0) {
        return false;
    }
    select_star(star);
    return true;
}

bool Starfield::project(const glm::vec3& p, float& x, float& y) const {
    // NiCamera WorldPtToScreenPt3 with the Stats camera's frustum.
    const glm::mat3 cam = unit(m_rotation);
    const glm::vec3 d = p - m_position;
    const float depth = glm::dot(d, cam[0]);
    if (depth <= 1e-5f) {
        return false;
    }
    const float v = std::tan(k_camera_fov * 0.017453292f) * 0.75f * 0.55f;
    x = 0.5f + glm::dot(d, cam[2]) / depth / (2.0f * v * m_aspect);
    y = 0.5f - glm::dot(d, cam[1]) / depth / (2.0f * v);
    return x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f;
}

void Starfield::labels(std::vector<StarLabel>& out) const {
    out.clear();
    for (const Star& s : m_stars) {
        float x = 0, y = 0;
        if (project(s.world, x, y)) {
            out.push_back({&m_trees[size_t(s.skill)].nodes[s.node], x, y});
        }
    }
}

bool Starfield::click_star(float x, float y, int& skill) {
    if (!m_built || m_state != state_tree) {
        return false;
    }
    const int count = int(m_trees.size());
    int best = -1;
    float best_d = 0;
    for (size_t i = 0; i < m_stars.size(); ++i) {
        const int away = std::abs(m_stars[i].skill - skill);
        float sx = 0, sy = 0;
        if ((away > 1 && away != count - 1) || !project(m_stars[i].world, sx, sy)) {
            continue;
        }
        const float d = std::hypot(sx - x, sy - y);
        if (best < 0 || d < best_d) {
            best = int(i);
            best_d = d;
        }
    }
    if (best < 0) {
        return false;
    }
    if (best == m_perk_star) {
        return y < 0.85f && best_d <= 0.075f; // not in the bottom 15%, and on it
    }
    skill = m_stars[size_t(best)].skill;
    select_star(best);
    return false;
}

void Starfield::update_states(std::vector<PerkTree> trees) {
    for (size_t i = 0; i < m_stars.size() && i < m_star_anims.size(); ++i) {
        const Star& s = m_stars[i];
        const int old = m_trees[size_t(s.skill)].nodes[s.node].state;
        const int next = trees[size_t(s.skill)].nodes[s.node].state;
        if (old != next) {
            m_star_anims[i]->deactivate_sequence(k_star_sequence[std::clamp(old, 0, 2)]);
            m_star_anims[i]->activate_sequence(k_star_sequence[std::clamp(next, 0, 2)]);
        }
    }
    m_trees = std::move(trees);
    for (size_t i = 0; i < m_lines.size() && i < m_line_anims.size(); ++i) {
        Line& l = m_lines[i];
        const Star& s = m_stars[l.star];
        const PerkTree& t = m_trees[size_t(s.skill)];
        const PerkTree::Node& n = t.nodes[s.node];
        const auto it = std::find_if(t.nodes.begin(), t.nodes.end(), [&](const PerkTree::Node& c) {
            return l.child < n.children.size() && c.index == n.children[l.child];
        });
        const bool unlocked = n.state == 2 && it != t.nodes.end() && it->state == 2;
        if (unlocked != l.unlocked) {
            m_line_anims[i]->deactivate_sequence(k_line_sequence[l.unlocked ? 1 : 0]);
            m_line_anims[i]->activate_sequence(k_line_sequence[unlocked ? 1 : 0]);
            l.unlocked = unlocked;
        }
    }
}

void Starfield::leave_tree() {
    if (!m_built || m_state != state_tree) {
        return;
    }
    // Back to the ring: the origin, looking at the selected skill's Point, 1000 ms; the image fades in on
    // arrival.
    m_state = state_ring;
    m_perk_star = -1;
    m_pose = ring_pose(m_selected);
    m_pose_direction = m_direction;
    start_move(m_pose, glm::vec3(0.0f), k_tree_ms);
}

void Starfield::close() {
    if (!m_built || m_state == state_closing) {
        return;
    }
    // Idle and CameraIntro off, CameraOutro on (from its start once the intro has played); the camera follows
    // CameraPosition again with the rotation it has (ring or tree).
    m_dome_anim->deactivate_sequence("Idle");
    m_dome_anim->deactivate_sequence("CameraIntro");
    m_dome_anim->activate_sequence("CameraOutro", 1.0f, 0.0f, 0);
    m_state = state_closing;
    m_moving = false;
}

void Starfield::update(double now_ms, int selected, double direction) {
    m_now = now_ms;
    m_selected = selected;
    const float w = float(m_ui.target_width()), h = float(m_ui.target_height());
    m_aspect = w / std::max(h, 1.0f);
    if (!m_built) {
        m_direction = direction;
        auto& models = m_ui.scene_models();
        m_dome = m_dome ? m_dome : models.get(m_beast ? k_beast_skydome : k_skydome);
        m_star = m_star ? m_star : models.get(k_star);
        m_line = m_line ? m_line : models.get(k_line);
        if (m_dome && m_star && m_line) {
            build();
        }
        return;
    }
    // The skydome's controllers at the game clock (seconds since the scene was built).
    m_dome_anim->update(float((m_now - m_intro_start) * 0.001));
    if (m_state == state_closing) {
        if (const uint32_t cp = m_dome_anim->find_node("CameraPosition"); cp != ose::k_none) {
            m_position = m_dome_anim->node_world_translation_units(cp);
        }
    }
    if (m_state == state_opening) {
        // The camera at CameraPosition's world translation, with the ring pose.
        const uint32_t cp = m_dome_anim->find_node("CameraPosition");
        if (cp != ose::k_none) {
            m_position = m_dome_anim->node_world_translation_units(cp);
        }
        const float t = m_dome_anim->sequence_time("CameraIntro");
        const float end = m_dome_anim->sequence_duration("CameraIntro");
        if (t < 0.0f || t >= end) {
            // The intro's end: Idle, and the 300 ms settle onto the ring pose.
            m_dome_anim->deactivate_sequence("CameraIntro");
            m_dome_anim->activate_sequence("Idle", 1.0f, 0.0f, 0);
            m_state = state_ring;
            m_pose = ring_pose(m_selected);
            m_pose_direction = direction;
            start_move(m_pose, glm::vec3(0.0f), k_settle_ms);
        }
    }
    // The ring's turn: the camera pre-multiplied by a turn about world Z, by the change of the direction the
    // menu sent (SetDirection = 360 - yaw, the yaw's change is the turn).
    if (m_state == state_ring && !m_moving) {
        const double turn = (direction - m_pose_direction) * 0.017453292519943295;
        const float c = float(std::cos(turn)), s = float(std::sin(turn));
        const glm::mat3 rz(glm::vec3(c, -s, 0), glm::vec3(s, c, 0),
                           glm::vec3(0, 0, 1)); // rows [c s 0; -s c 0; 0 0 1]
        m_rotation = rz * m_pose;
    }
    m_direction = direction;
    if (m_moving) {
        // The game: in a tree (1 - cos pi t) / 2, otherwise sin(pi t / 2).
        double t = m_move_ms > 0 ? (m_now - m_move_start) / m_move_ms : 1.0;
        double a = m_state == state_tree ? (1.0 - std::cos(3.14159265358979 * t)) * 0.5
                                         : (t < 1.0 ? std::sin(1.5707963267948966 * t) : t);
        if (a > 1.0 || m_now > m_move_start + m_move_ms) {
            a = 1.0;
        }
        m_rotation = glm::mat3_cast(glm::slerp(m_q0, m_q1, float(a)));
        m_position = m_p0 + (m_p1 - m_p0) * float(a);
        if (a >= 1.0) {
            m_moving = false;
            if (m_state == state_ring) {
                fade(m_selected, 1); // arrived on the ring: the image fades in (UISkillsGlow)
                m_pose = m_rotation;
                m_pose_direction = direction;
            }
        }
    }
    // The stars and lines once the opening has played; their graphs advance by the real frame time.
    if (m_state != state_opening && !m_attached) {
        attach();
    } else if (m_attached) {
        const float dt = float(std::max(m_now - m_anim_now, 0.0) * 0.001);
        m_anim_now = m_now;
        for (auto& a : m_star_anims) {
            a->advance(dt);
        }
        for (auto& a : m_line_anims) {
            a->advance(dt);
        }
    }
    // The images' fades, 500 ms, a fade out always from 1.
    for (Fade& f : m_fades) {
        if (f.state == 0) {
            continue;
        }
        const double e = m_now - f.start;
        if (e > k_fade_ms) {
            f.alpha = f.state == 1 ? 1.0f : 0.0f;
            f.state = 0;
        } else {
            const float s = float(e / k_fade_ms);
            f.alpha = f.state == 1 ? s : 1.0f - s;
        }
    }
}

float Starfield::order_key(uint32_t dome_node, float distance_sq) const {
    // Blended draws go farthest (largest key) first: the ordered node's children in their order, by distance
    // inside one child's subtree.
    const int j = dome_node < m_order.size() ? m_order[dome_node] : -1;
    if (j < 0) {
        return -1.0f;
    }
    return float(m_order_count - j) * 1.0e5f + std::min(distance_sq, 9.0e4f);
}

void Starfield::add_billboarded(UiScene& out, uint32_t model, const SceneModel& m,
                                const fx::EffectInstance* anim, const std::vector<int>& materials,
                                const glm::mat3& root_rotation, float root_scale, const glm::vec3& root_t,
                                const glm::mat3& cam, float sort) const {
    // The model shape by shape, its nodes where its sequences put them; a shape under an NiBillboardNode
    // turned to the camera as the cull turns the node (
    //). Gamebryo keeps a node's world rotation (which need not be
    // orthonormal: the lines') and its uniform scale apart; so does this.
    const uint32_t root = m.graph.root;
    const Affine root_rest = rest(m, root);
    auto below_root = [&](uint32_t n) {
        return anim ? animated(*anim, root).inverse() * animated(*anim, n) : root_rest.inverse() * rest(m, n);
    };
    auto scale_below_root = [&](uint32_t n) {
        return anim ? anim->node_world_scale(n) / std::max(anim->node_world_scale(root), 1e-12f)
                    : m.rest_scale[n] / std::max(m.rest_scale[root], 1e-12f);
    };
    const Affine root_world{root_rotation * root_scale, root_t};
    for (size_t i = 0; i < m.graph.submesh_nodes.size(); ++i) {
        const uint32_t node = m.graph.submesh_nodes[i];
        if (node == ose::k_none) {
            continue;
        }
        const Affine shape = below_root(node);
        Affine world = root_world * shape;
        // What the .osm's vertices hold: the shape's rest transform in the model's space, or below its billboard.
        Affine baked = root_rest.inverse() * rest(m, node);
        bool turned = false;
        for (uint32_t b = node; b != ose::k_none; b = m.graph.nodes[b].parent) {
            if (m.graph.node_extras[b].kind != ose::NodeKind::billboard) {
                continue;
            }
            const Affine rel = below_root(b);
            const float rel_scale = scale_below_root(b);
            const glm::mat3 w = root_rotation * (rel.r / std::max(rel_scale, 1e-12f));
            const float s = root_scale * rel_scale;
            const glm::vec3 p = root_t + root_scale * (root_rotation * rel.t);
            const glm::mat3 r = billboard(m.graph.node_extras[b].billboard_mode, w, p, s, m_position, cam);
            world = Affine{r * s, p} * (rel.inverse() * shape);
            baked = rest(m, b).inverse() * rest(m, node);
            turned = true;
            break;
        }
        const Affine t = world * baked.inverse();
        SceneDraw d;
        d.model = model;
        d.submesh = int(i);
        d.sort = sort;
        d.turned = turned;
        put(d.rotation, t.r);
        for (int k = 0; k < 3; ++k) {
            d.translation[k] = t.t[k];
        }
        if (anim && i < materials.size() && materials[i] >= 0) {
            put_material(d, anim->material(size_t(materials[i])));
        }
        out.draws.push_back(d);
    }
}

bool Starfield::scene(UiScene& out) const {
    if (!m_built) {
        return false;
    }
    out.slot = 6;
    // The game: t = tan(fStatsCameraFOV in radians), half slopes 0.75 x 0.55 t up
    // and that x W / H across, near fStatsCameraNearDistance.
    const float t = std::tan(k_camera_fov * 0.017453292f);
    const float v = t * 0.75f * 0.55f;
    out.camera.frustum[0] = -v * m_aspect;
    out.camera.frustum[1] = v * m_aspect;
    out.camera.frustum[2] = v;
    out.camera.frustum[3] = -v;
    out.camera.frustum[4] = k_camera_near;
    out.camera.frustum[5] = 20480.0f;
    put(out.camera.rotation, m_rotation);
    for (int k = 0; k < 3; ++k) {
        out.camera.position[k] = m_position[k];
    }
    const glm::mat3 cam = unit(m_rotation);
    // The skydome shape by shape, its animated nodes where the sequences put them, the constellation images
    // at their fade's alpha.
    const SceneModel& dome = *m_dome;
    const uint32_t dome_model = out.model(m_beast ? k_beast_skydome : k_skydome);
    const Affine root{dome.rest_rotation[dome.graph.root], glm::vec3(0.0f)};
    const Affine draw = model_draw(dome, root);
    for (size_t i = 0; i < dome.graph.submesh_nodes.size(); ++i) {
        const uint32_t node = dome.graph.submesh_nodes[i];
        if (node == ose::k_none) {
            continue;
        }
        // The node's animated world against its rest: the shape moves with it.
        Affine anim{glm::mat3(1.0f), glm::vec3(0.0f)};
        if (m_dome_anim) {
            const glm::mat3 ar = m_dome_anim->node_world_rotation(node) * m_dome_anim->node_world_scale(node);
            const glm::vec3 at = m_dome_anim->node_world_translation_units(node);
            const Affine rest_world = rest(dome, node);
            anim = Affine{ar, at} * rest_world.inverse();
        }
        const Affine t2 = Affine{root.r, root.t} * anim * Affine{root.r, root.t}.inverse() * draw;
        SceneDraw d;
        d.model = dome_model;
        d.submesh = int(i);
        put(d.rotation, t2.r);
        for (int k = 0; k < 3; ++k) {
            d.translation[k] = t2.t[k];
        }
        d.sort = order_key(node, 0.0f);
        if (m_dome_anim && i < m_dome_materials.size() && m_dome_materials[i] >= 0) {
            put_material(d, m_dome_anim->material(size_t(m_dome_materials[i]))); // the gases' V scroll (Idle)
        }
        const std::string_view name = dome.graph.string(dome.graph.nodes[node].name);
        if (name.size() == 8 && name.starts_with("cImage")) {
            d.alpha = image_alpha((name[6] - '0') * 10 + (name[7] - '0'));
        }
        out.draws.push_back(d);
    }
    if (!m_attached) {
        return true; // the stars attach once the opening has played
    }
    const uint32_t star_model = out.model(k_star);
    const uint32_t line_model = out.model(k_line);
    // A star: the template's clone under its Point, its local translation the star's, its scale
    // fStatsStarScale; a line under its parent star.
    const glm::mat3 star_rest = unit(m_star->rest_rotation[m_star->graph.root]);
    const float m2 = units::to_metres(1.0f) * units::to_metres(1.0f);
    for (size_t i = 0; i < m_stars.size(); ++i) {
        const Star& s = m_stars[i];
        const glm::mat3 r = unit(m_point_rot[size_t(s.skill)]) * star_rest;
        const glm::vec3 rel = s.world - m_position;
        add_billboarded(out, star_model, *m_star, i < m_star_anims.size() ? m_star_anims[i].get() : nullptr,
                        m_star_materials, r, k_star_scale, s.world, cam,
                        order_key(m_point_node[size_t(s.skill)], m2 * glm::dot(rel, rel)));
    }
    for (size_t i = 0; i < m_lines.size(); ++i) {
        const Line& l = m_lines[i];
        const Star& s = m_stars[l.star];
        const glm::mat3 r = unit(m_point_rot[size_t(s.skill)]) * star_rest * l.rotation;
        const glm::vec3 rel = s.world - m_position;
        add_billboarded(out, line_model, *m_line, i < m_line_anims.size() ? m_line_anims[i].get() : nullptr,
                        m_line_materials, r, k_star_scale * l.scale, s.world, cam,
                        order_key(m_point_node[size_t(s.skill)], m2 * glm::dot(rel, rel)));
    }
    return true;
}

std::string Starfield::state_json() const {
    auto vec = [](const glm::vec3& v) { return std::format("[{:.9g}, {:.9g}, {:.9g}]", v.x, v.y, v.z); };
    std::string rot;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            rot += std::format("{}{:.9g}", r || c ? ", " : "", m_rotation[c][r]);
        }
    }
    std::string j =
        std::format("{{\"built\": {}, \"state\": {}, \"selected\": {}, \"moving\": {}, \"ring_axis\": {}, "
                    "\"camera\": {{\"translate\": {}, \"rotate\": [{}]}}, \"stars\": {}, \"lines\": {}, "
                    "\"alphas\": [",
                    m_built, int(m_state), m_selected, m_moving, vec(m_axis), vec(m_position), rot,
                    m_stars.size(), m_lines.size());
    for (size_t i = 0; i < m_fades.size(); ++i) {
        j += std::format("{}{:.9g}", i ? ", " : "", m_fades[i].alpha);
    }
    j += "], \"star_positions\": [";
    for (size_t i = 0; i < m_stars.size(); ++i) {
        j += std::format("{}{{\"skill\": {}, \"local\": {}, \"world\": {}}}", i ? ", " : "", m_stars[i].skill,
                         vec(m_stars[i].local), vec(m_stars[i].world));
    }
    return j + "]}";
}

} // namespace spud::ui
