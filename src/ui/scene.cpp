#include "ui/scene.hpp"

#include "core/error.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"

#include <cmath>
#include <cstring>
#include <set>
#include <unordered_map>

namespace spud::ui {

SceneCamera default_scene_camera(float width, float height) {
    SceneCamera c;
    const float t = std::tan(80.0f * 0.0026179939f);
    const float h = 0.75f * t;
    const float v = h * (height / std::max(width, 1.0f));
    c.frustum[0] = -h;
    c.frustum[1] = h;
    c.frustum[2] = v;
    c.frustum[3] = -v;
    c.frustum[4] = 15.0f;
    c.frustum[5] = 20480.0f;
    return c;
}

void merge_bound(glm::vec3& center, float& radius, const glm::vec3& other_center, float other_radius) {
    // The game: one inside the other keeps the larger; otherwise the sphere through both
    // far ends.
    const glm::vec3 d = center - other_center;
    const float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
    const float diff = other_radius - radius;
    const float diff2 = diff * diff;
    if (diff < 0.0f) {
        if (diff2 >= d2) {
            return;
        }
    } else if (diff2 >= d2) {
        center = other_center;
        radius = other_radius;
        return;
    }
    const float len = std::sqrt(d2);
    if (len > 1e-6f) {
        const float k = (len - diff) * (0.5f / len);
        center = other_center + d * k;
    }
    radius = ((other_radius + len) + radius) * 0.5f;
}

uint32_t SceneModel::find(std::string_view name) const {
    for (uint32_t i = 0; i < graph.nodes.size(); ++i) {
        if (graph.string(graph.nodes[i].name) == name) {
            return i;
        }
    }
    return ose::k_none;
}

int SceneModel::submesh_of(uint32_t node) const {
    for (size_t i = 0; i < graph.submesh_nodes.size(); ++i) {
        if (graph.submesh_nodes[i] == node) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<uint32_t> SceneModel::children(uint32_t node) const {
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < graph.nodes.size(); ++i) {
        if (graph.nodes[i].parent == node) {
            out.push_back(i);
        }
    }
    return out;
}

namespace {

glm::mat3 node_rotation(const ose::Node& n) {
    glm::mat3 r;
    std::memcpy(&r, n.rotation, sizeof r); // column-major as stored
    return r;
}

} // namespace

void SceneModel::world_bound(uint32_t node, const glm::mat3& root_rotation, const glm::vec3& root_translation,
                             glm::vec3& center, float& radius, uint32_t detached) const {
    // World transforms from the root placed as given: world = parent.R * (local R * s) , parent.R * t +
    // parent.t.
    const size_t n = graph.nodes.size();
    std::vector<glm::mat3> rot(n);
    std::vector<glm::vec3> pos(n);
    std::vector<uint8_t> done(n, 0);
    auto world = [&](auto& self, uint32_t i) -> void {
        if (done[i]) {
            return;
        }
        const ose::Node& nd = graph.nodes[i];
        if (i == graph.root || nd.parent == ose::k_none) {
            rot[i] = root_rotation;
            pos[i] = root_translation;
        } else {
            self(self, nd.parent);
            rot[i] = rot[nd.parent] * (node_rotation(nd) * nd.scale);
            pos[i] = rot[nd.parent] * glm::vec3(nd.translation[0], nd.translation[1], nd.translation[2]) +
                     pos[nd.parent];
        }
        done[i] = 1;
    };
    // The game: radius 0, the first child with a bound copied, every later one
    // merged.
    auto bound = [&](auto& self, uint32_t i, glm::vec3& c, float& r) -> void {
        c = glm::vec3(0.0f);
        r = 0.0f;
        const ose::NodeExtra* x = i < graph.node_extras.size() ? &graph.node_extras[i] : nullptr;
        if (x && x->kind == ose::NodeKind::shape) {
            world(world, i);
            const float s = std::cbrt(std::abs(glm::determinant(rot[i])));
            c = rot[i] * glm::vec3(x->bound_center[0], x->bound_center[1], x->bound_center[2]) + pos[i];
            r = x->bound_radius * s;
            return;
        }
        bool first = true;
        for (uint32_t k : children(i)) {
            if (k == detached) {
                continue;
            }
            glm::vec3 cc;
            float cr;
            self(self, k, cc, cr);
            if (cr == 0.0f) {
                continue;
            }
            if (first) {
                c = cc;
                r = cr;
                first = false;
            } else {
                merge_bound(c, r, cc, cr);
            }
        }
    };
    bound(bound, node, center, radius);
}

// ---- loading -------------------------------------------------------------------------------------

struct SceneModels::Shared {
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<const SceneModel>> ready;
    std::set<std::string> pending;
    std::set<std::string> failed;
};

SceneModels::SceneModels(std::filesystem::path cache_root, std::function<void(std::function<void()>)> submit)
    : m_root(std::move(cache_root)), m_submit(std::move(submit)), m_shared(std::make_shared<Shared>()) {}

SceneModels::~SceneModels() = default;

bool SceneModels::failed(const std::string& osm_path) const {
    std::lock_guard lock(m_shared->mutex);
    return m_shared->failed.contains(osm_path);
}

std::shared_ptr<const SceneModel> SceneModels::get(const std::string& osm_path) {
    {
        std::lock_guard lock(m_shared->mutex);
        if (auto it = m_shared->ready.find(osm_path); it != m_shared->ready.end()) {
            return it->second;
        }
        if (m_shared->pending.contains(osm_path) || m_shared->failed.contains(osm_path)) {
            return nullptr;
        }
        m_shared->pending.insert(osm_path);
    }
    // Read and parse off the render thread; the menus see the model from a later frame.
    m_submit([shared = m_shared, root = m_root, osm_path] {
        std::shared_ptr<SceneModel> m;
        try {
            m = std::make_shared<SceneModel>();
            m->osm = osm_path;
            m->graph = ose::read(read_file(root / path::replace_extension(osm_path, ".ose")));
            if (m->graph.node_extras.empty()) {
                throw Error("no scene graph (convert it again: skytool ui import, or skytool ui model)");
            }
            const osm::Mesh mesh = osm::read(read_file(root / osm_path));
            for (const auto& sm : mesh.submeshes) {
                m->submesh_textures.push_back(sm.material < mesh.materials.size()
                                                  ? mesh.materials[sm.material].textures
                                                  : std::array<std::string, osm::k_texture_slots>{});
            }
            // Rest transforms in the model's space, the root's own transform included.
            const size_t n = m->graph.nodes.size();
            m->rest_rotation.assign(n, glm::mat3(1.0f));
            m->rest_translation.assign(n, glm::vec3(0.0f));
            m->rest_scale.assign(n, 1.0f);
            std::vector<uint8_t> done(n, 0);
            auto rest = [&](auto& self, uint32_t i) -> void {
                if (done[i]) {
                    return;
                }
                const ose::Node& nd = m->graph.nodes[i];
                const glm::mat3 local = node_rotation(nd) * nd.scale;
                const glm::vec3 t(nd.translation[0], nd.translation[1], nd.translation[2]);
                if (nd.parent == ose::k_none) {
                    m->rest_rotation[i] = local;
                    m->rest_translation[i] = t;
                    m->rest_scale[i] = nd.scale;
                } else {
                    self(self, nd.parent);
                    m->rest_rotation[i] = m->rest_rotation[nd.parent] * local;
                    m->rest_translation[i] = m->rest_rotation[nd.parent] * t + m->rest_translation[nd.parent];
                    m->rest_scale[i] = m->rest_scale[nd.parent] * nd.scale;
                }
                done[i] = 1;
            };
            for (uint32_t i = 0; i < n; ++i) {
                rest(rest, i);
            }
        } catch (const std::exception& e) {
            log::warn("ui scene: {}: {}", osm_path, e.what());
            m.reset();
        }
        std::lock_guard lock(shared->mutex);
        shared->pending.erase(osm_path);
        if (m) {
            shared->ready.emplace(osm_path, std::move(m));
        } else {
            shared->failed.insert(osm_path);
        }
    });
    return nullptr;
}

} // namespace spud::ui
