#include "ui/lock3d.hpp"

#include "ui/ui.hpp"

#include "core/units.hpp"
#include "formats/osm.hpp"
#include "fx/effect.hpp"

#include <algorithm>
#include <cmath>

namespace spud::ui {

namespace {

// The Interface settings the menu reads.
constexpr float k_lock_max_angle = 90.0f;     // fLockMaxAngle
constexpr float k_centre_offset = 9.0f;       // fLockCenterOffset
constexpr float k_rot_centre_x = -14.5f;      // fLockRotCenterOffsetX
constexpr float k_rot_centre_z = 3.0f;        // fLockRotCenterOffsetZ
constexpr float k_position_x = 0.0f;          // fLockPositionX
constexpr float k_position_z = 3.0f;          // fLockPositionZ
constexpr float k_degrees = 0.017453292f;

constexpr const char* k_lock_model = "meshes/interface/lockpicking/lockpickshiv01.osm";
constexpr const char* k_pick_model = "meshes/interface/lockpicking/lockpick01.osm";
constexpr const char* k_key_model = "meshes/interface/lockpicking/lockpickskeletonkey01.osm";

// A row-major matrix as the design writes them.
glm::mat3 rows(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    return glm::transpose(glm::mat3(a, b, c));
}

// Rz(180 degrees), both roots' rotation in SetupModels.
glm::mat3 turned() {
    return rows({-1, 0, 0}, {0, -1, 0}, {0, 0, 1});
}

void put(float (&out)[9], const glm::mat3& m) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r * 3 + c] = m[c][r];
        }
    }
}

} // namespace

struct Lock3D::Held {
    std::string osm;
    std::shared_ptr<const SceneModel> graph;
    std::unique_ptr<fx::EffectInstance> anim;
    std::string sequence; // the one on
    float time = 0, begin = 0, end = 0, clock = 0;
    bool running = false; // moves with the frame's time; else the menu poses it
    bool ended = false;

    void pose() {
        if (!sequence.empty()) {
            anim->pose_sequence(sequence, time);
        }
    }
};

Lock3D::Lock3D(Ui& ui) : m_ui(ui) {}
Lock3D::~Lock3D() = default;

void Lock3D::open(bool skeleton_key) {
    m_lock = std::make_unique<Held>();
    m_pick = std::make_unique<Held>();
    m_lock->osm = k_lock_model;
    m_pick->osm = skeleton_key ? k_key_model : k_pick_model;
    settled();
}

bool Lock3D::settled() {
    if (!m_lock) {
        return true;
    }
    bool done = true;
    for (Held* h : {m_lock.get(), m_pick.get()}) {
        if (!h->graph) {
            h->graph = m_ui.scene_models().get(h->osm);
        }
        done = done && (h->graph || m_ui.scene_models().failed(h->osm));
    }
    return done;
}

void Lock3D::place(Held& h, const glm::mat3& rotation, const glm::vec3& translation) {
    const ose::Effect& e = h.anim->data();
    h.anim->set_root_transform(rotation, translation * units::k_skyrim_unit_m, e.nodes[e.root].scale);
}

void Lock3D::play(Held& h, const char* sequence, bool running) {
    if (!h.anim) {
        return;
    }
    if (!h.sequence.empty()) {
        h.anim->deactivate_sequence(h.sequence);
        h.sequence.clear();
    }
    h.running = false;
    h.ended = false;
    if (h.anim->sequence_range(sequence, h.begin, h.end) && h.anim->activate_sequence(sequence)) {
        h.sequence = sequence;
        h.time = h.begin;
        h.running = running;
    }
}

void Lock3D::start() {
    const float aspect = float(m_ui.target_width()) / std::max(float(m_ui.target_height()), 1.0f);
    m_y = aspect > 1.3333334f ? -1300.0f : -1100.0f; // fLockPositionYWide, fLockPositionY
    const float alpha = k_lock_max_angle * k_degrees;
    m_pivot = {k_position_x + k_centre_offset * std::cos(alpha), m_y, k_position_z - k_centre_offset * std::sin(alpha)};
    for (Held* h : {m_lock.get(), m_pick.get()}) {
        if (h && h->graph) {
            h->anim = std::make_unique<fx::EffectInstance>(fx::make_effect(h->graph->graph));
            place(*h, turned(), {k_position_x, m_y, k_position_z});
        }
    }
    if (m_lock) {
        play(*m_lock, "Intro", true);
        play(*m_pick, "Intro", true);
    }
    m_started = true;
}

Lock3D::Ended Lock3D::update(double dt) {
    Ended out;
    if (!m_started || !m_lock) {
        return out;
    }
    for (Held* h : {m_lock.get(), m_pick.get()}) {
        if (!h->anim) {
            continue;
        }
        h->clock += float(dt);
        if (h->running) {
            h->time = std::min(h->time + float(dt), h->end);
            if (h->time >= h->end && !h->ended) {
                h->ended = true;
                (h == m_lock.get() ? out.lock_intro : h->sequence == "Intro" ? out.pick_intro : out.pick_break) = true;
            }
        }
        h->pose();
        h->anim->update(h->clock);
    }
    if (out.lock_intro) {
        play(*m_lock, "Rotate", false); // activated, and never advanced by time
    }
    if (out.pick_intro) {
        play(*m_pick, "PickDamage", false);
    }
    return out;
}

void Lock3D::pose_lock(double angle) {
    if (m_lock && m_lock->anim && m_lock->sequence == "Rotate") {
        m_lock->time = m_lock->begin + (m_lock->end - m_lock->begin) * float(angle) / k_lock_max_angle;
        m_lock->time = std::clamp(m_lock->time, m_lock->begin, m_lock->end);
    }
}

void Lock3D::pose_pick(double angle, double lock_angle) {
    if (!m_pick || !m_pick->anim) {
        return;
    }
    const float beta = float(angle) * k_degrees, alpha = (k_lock_max_angle - float(lock_angle)) * k_degrees;
    const float c = std::cos(beta), s = std::sin(beta);
    const glm::mat3 m = rows({c, 0, -s}, {0, 1, 0}, {s, 0, c});
    const float dx = k_rot_centre_x - m_pivot.x, dz = k_rot_centre_z - m_pivot.z;
    const glm::vec3 at{k_rot_centre_x - (dx * c - dz * s) - k_centre_offset * std::cos(alpha), m_pivot.y,
                       k_rot_centre_z - (dx * s + dz * c) + k_centre_offset * std::sin(alpha)};
    place(*m_pick, m * turned(), at);
}

void Lock3D::pose_damage(double health) {
    if (m_pick && m_pick->anim && m_pick->sequence == "PickDamage") {
        m_pick->time = m_pick->end - (m_pick->end - m_pick->begin) * float(health) / 100.0f;
    }
}

void Lock3D::break_pick() {
    if (m_pick) {
        play(*m_pick, "PickBreak", true);
    }
}

void Lock3D::insert_pick() {
    if (m_pick) {
        play(*m_pick, "Intro", true);
    }
}

void Lock3D::draw(const Held& h, UiScene& out) const {
    if (!h.anim) {
        return;
    }
    const SceneModel& m = *h.graph;
    const uint32_t model = out.model(h.osm);
    const uint32_t root = m.graph.root;
    // The .osm holds each shape where its node rests below the root, the root's own transform left out.
    const glm::mat3 root_r = glm::inverse(m.rest_rotation[root]);
    const glm::vec3 root_t = -(root_r * m.rest_translation[root]);
    for (size_t i = 0; i < m.graph.submesh_nodes.size(); ++i) {
        const uint32_t node = m.graph.submesh_nodes[i];
        if (node == ose::k_none) {
            continue;
        }
        const glm::mat3 rest_r = root_r * m.rest_rotation[node];
        const glm::vec3 rest_t = root_r * m.rest_translation[node] + root_t;
        const glm::mat3 world_r = h.anim->node_world_rotation(node) * h.anim->node_world_scale(node);
        const glm::vec3 world_t = h.anim->node_world_translation_units(node);
        const glm::mat3 r = world_r * glm::inverse(rest_r);
        const glm::vec3 t = world_t - r * rest_t;
        SceneDraw d;
        d.model = model;
        d.submesh = int(i);
        put(d.rotation, r);
        for (int k = 0; k < 3; ++k) {
            d.translation[k] = t[k];
        }
        out.draws.push_back(d);
    }
}

bool Lock3D::scene(UiScene& out) const {
    if (!m_started || !m_lock || !m_lock->anim) {
        return false;
    }
    out.slot = 7;
    out.camera = default_scene_camera(float(m_ui.target_width()), float(m_ui.target_height()));
    // fLockpickLight*, the game.
    out.lights.push_back({{100.0f, -1000.0f, 100.0f}, {0.98f, 0.95f, 0.83f}, 400.0f, 1.75f, false});
    draw(*m_lock, out);
    draw(*m_pick, out);
    return true;
}

} // namespace spud::ui
