#include "engine/tree_sway.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/units.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <numbers>

namespace spud {

namespace {

constexpr float k_unit_m = units::k_skyrim_unit_m;

// The engine's sine and cosine (lighting.md 12.4, the notes' tree sway report 1.5): the argument reduced to [-pi, pi]
// by rounding a / 2 pi to the nearest integer, then odd and even polynomials.
float reduce(float a) { return a - 6.2831855f * std::nearbyint(a * 0.15915494f); }
float engine_sin(float a) {
    const float r = reduce(a), r2 = r * r;
    return r + r * r2 * (-0.16652186f + r2 * (0.008199913f + r2 * -0.00016147594f));
}
float engine_cos(float a) {
    const float r = reduce(a), r2 = r * r;
    return 1.0f + r2 * (-0.49534801f + r2 * (0.0387826f + r2 * -0.00092458795f));
}

// The wave's mode 2: cos(pi x) cos(3 pi x) cos(5 pi x) cos(7 pi x) - 0.1 sin(25 pi x), a gust near every integer x.
float branch_wave(float x) {
    const float c = engine_cos(3.1415927f * x) * engine_cos(9.424778f * x) * engine_cos(15.707964f * x) *
                    engine_cos(21.99115f * x);
    return c - 0.1f * engine_sin(78.539818f * x);
}

// The phase offset of bone i: cos(i) from the game's 512-entry table, i in radians.
float table_cos(uint32_t i) {
    const uint32_t k = static_cast<uint32_t>(static_cast<float>(i) * 81.487328f) & 511u;
    return std::cos(static_cast<float>(k) * (2.0f * std::numbers::pi_v<float> / 512.0f));
}

// The tree's random starting timer in [0, 2 pi), the same hash of the reference as the leaves' (mesh_transform.glsl):
// the game draws it once per reference and shares it between leaves and branches.
float tree_phase(uint32_t id) {
    uint32_t h = id * 747796405u + 2891336453u;
    h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    h = (h >> 22u) ^ h;
    return static_cast<float>(h >> 8) * (6.2831855f / 16777216.0f);
}

// NiQuaternion from angle and axis, the axis as given (not normalised again): w = cos(a/2), xyz = sin(a/2) axis.
glm::quat angle_axis(float angle, const glm::vec3& axis) {
    const float s = std::sin(angle * 0.5f);
    return glm::quat(std::cos(angle * 0.5f), axis.x * s, axis.y * s, axis.z * s);
}

// NiQuaternion's fast slerp: a normalised lerp with a correction of t, mirrored above 0.5.
glm::quat fast_slerp(const glm::quat& a, const glm::quat& b, float t) {
    const float d = (a.x * b.x + a.w * b.w + a.y * b.y + a.z * b.z) * 0.82279688f;
    const float k = (1.0f - d) * (1.0f - d) * 0.58549219f;
    float u;
    if (t > 0.5f) {
        const float s = 1.0f - t;
        u = 1.0f - ((((s + s) - 3.0f) * (k * s) + 1.0f) + k) * s;
    } else {
        u = ((((t + t) - 3.0f) * (k * t) + 1.0f) + k) * t;
    }
    glm::quat q(a.w + (b.w - a.w) * u, a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u);
    const float n = 1.0f / std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    return glm::quat(q.w * n, q.x * n, q.y * n, q.z * n);
}

glm::mat3 rows_to_mat(const float rows[3][3]) {
    glm::mat3 m;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            m[c][r] = rows[r][c];
        }
    }
    return m;
}

glm::mat4 rows_to_mat4(const float rows[3][4]) {
    glm::mat4 m(1.0f);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            m[c][r] = rows[r][c];
        }
    }
    return m;
}

struct World {
    glm::mat3 rotation{1.0f};
    glm::vec3 translation{0.0f};
    float scale = 1.0f;
};

} // namespace

TreeSway::TreeSway(render::MeshManager& meshes, std::filesystem::path cache) : m_meshes(meshes), m_cache(std::move(cache)) {}

TreeSway::~TreeSway() {
    for (auto& [ref, t] : m_trees) {
        free(t);
    }
    for (Model& m : m_models) {
        if (m.loading.valid()) {
            m.loading.wait();
        }
        if (m.mesh != render::k_invalid_mesh) {
            m_meshes.release(m.mesh);
        }
    }
}

TreeSway::Model* TreeSway::model(const std::string& mesh, uint32_t& index) {
    auto [it, fresh] = m_model_index.try_emplace(mesh, static_cast<uint32_t>(m_models.size()));
    index = it->second;
    if (fresh) {
        Model& m = m_models.emplace_back();
        const std::string base = mesh.substr(0, mesh.size() - 4);
        m.mesh = m_meshes.acquire(base + ".sway.osm", 1.0f);
        m.loading = std::async(std::launch::async, [file = m_cache / (base + ".sway.osk")] {
            return osk::read(read_file(file));
        });
    }
    return &m_models[index];
}

// Ready to pose: the skinned copy loaded, the bones read and laid out.
bool TreeSway::prepare(Model& m) {
    if (m.ready || m.failed) {
        return m.ready;
    }
    if (m.mesh == render::k_invalid_mesh || m_meshes.failed(m.mesh)) {
        m.failed = true;
        return false;
    }
    if (!m_meshes.ready(m.mesh) || m.loading.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return false;
    }
    const render::SkinnedModel* skinned = m_meshes.skinned_model(m.mesh);
    osk::Skeleton skeleton;
    try {
        skeleton = m.loading.get();
    } catch (const std::exception& e) {
        log::warn("tree sway: {}", e.what());
    }
    if (!skinned || skeleton.bones.empty()) {
        m.failed = true;
        return false;
    }
    std::vector<World> rest(skeleton.bones.size());
    m.nodes.resize(skeleton.bones.size());
    std::vector<std::pair<float, int32_t>> branches;
    for (size_t i = 0; i < skeleton.bones.size(); ++i) {
        const osk::Bone& b = skeleton.bones[i];
        m.names.push_back(b.name);
        Node& n = m.nodes[i];
        n.parent = b.parent;
        n.rotation = rows_to_mat(b.rotation);
        n.translation = glm::vec3(b.translation[0], b.translation[1], b.translation[2]);
        n.scale = b.scale;
        World& w = rest[i];
        if (b.parent >= 0) {
            const World& p = rest[size_t(b.parent)];
            w.rotation = p.rotation * n.rotation;
            w.scale = p.scale * n.scale;
            w.translation = p.translation + p.rotation * (p.scale * n.translation);
        }
        if (b.tree_branch) {
            branches.emplace_back(w.translation.z, static_cast<int32_t>(i));
        }
    }
    // The bones by their height, lowest first: the order the game sorts them in, which the phase and the fade follow.
    std::ranges::stable_sort(branches, {}, &std::pair<float, int32_t>::first);
    for (const auto& [z, node] : branches) {
        Branch br;
        br.node = node;
        const glm::vec3 x = rest[size_t(node)].rotation[0];
        const float length = std::hypot(x.x, x.y);
        br.direction = length > 1e-6f ? glm::vec2(x.x, x.y) / length : glm::vec2(0.0f);
        const int32_t parent = m.nodes[size_t(node)].parent;
        br.to_parent = parent >= 0 ? glm::transpose(rest[size_t(parent)].rotation) : glm::mat3(1.0f);
        m.branches.push_back(br);
    }
    m.skin_nodes.assign(skinned->bones.size(), -1);
    for (size_t b = 0; b < skinned->bones.size(); ++b) {
        for (size_t i = 0; i < skeleton.bones.size(); ++i) {
            if (skeleton.bones[i].name == skinned->bones[b].name) {
                m.skin_nodes[b] = static_cast<int32_t>(i);
                break;
            }
        }
    }
    m.ready = !m.branches.empty();
    m.failed = !m.ready;
    return m.ready;
}

TreeSway::Tree* TreeSway::tree(const Candidate& c, uint32_t model) {
    if (auto it = m_trees.find(c.form_id); it != m_trees.end()) {
        return &it->second;
    }
    const Model& m = m_models[model];
    const std::span<const uint32_t> plain = m_meshes.groups(c.plain, c.plain_variant);
    const std::span<const uint32_t> own = m_meshes.groups(m.mesh);
    Tree t;
    t.model = model;
    // Its own copy of the skinned model's vertices, with the materials the reference draws with (a snow tree's
    // directional material): the same textures, so the shape it was drawn with matches by its diffuse.
    t.variant = m_meshes.skinned_variant(
        m.mesh,
        [&](uint32_t k, render::MeshManager::IndexRuns& runs) {
            runs.push_back({0u, m_meshes.group(own[k]).index_count});
        },
        [&](uint32_t k, render::GpuMaterial& g) {
            const uint32_t diffuse = g.tex[render::gtex_diffuse];
            int64_t match = -1;
            if (plain.size() == own.size() && m_meshes.group_material(plain[k]).tex[render::gtex_diffuse] == diffuse) {
                match = k;
            }
            for (size_t j = 0; j < plain.size() && match < 0; ++j) {
                if (m_meshes.group_material(plain[j]).tex[render::gtex_diffuse] == diffuse) {
                    match = static_cast<int64_t>(j);
                }
            }
            if (match >= 0) {
                render::GpuMaterial drawn = m_meshes.group_material(plain[size_t(match)]);
                std::memcpy(drawn.xform, g.xform, sizeof g.xform); // the skinned copy is posed in the model's frame
                drawn.extra[1] = g.extra[1];
                g = drawn;
            }
        },
        t.targets);
    if (t.variant == render::k_no_variant) {
        return nullptr;
    }
    t.locals.resize(m.nodes.size());
    for (size_t i = 0; i < m.nodes.size(); ++i) {
        t.locals[i] = m.nodes[i].rotation;
    }
    return &m_trees.emplace(c.form_id, std::move(t)).first->second;
}

void TreeSway::free(Tree& t) {
    if (t.variant != render::k_no_variant) {
        m_meshes.free_skinned_variant(t.variant);
        t.variant = render::k_no_variant;
    }
}

void TreeSway::pose(Tree& t, const Model& m, const Candidate& c, const Settings& settings, float time,
                    const glm::vec3& camera, std::vector<render::SkinJob>& jobs, std::vector<glm::vec4>& rows) {
    // The wind blows north, always (TreeManager +0x70): its direction in the tree's own frame, not renormalised.
    const glm::mat3 placed = glm::mat3_cast(c.rotation);
    const glm::vec3 wind(0.0f, 1.0f, 0.0f);
    const float wx = glm::dot(placed[0], wind), wy = glm::dot(placed[1], wind);
    // The TREE's amplitudes and frequencies, as the game derives them from the branch flexibility F.
    const float b = std::clamp(0.04f * c.branch_flexibility, 0.0f, 0.4f);
    const float front = 0.75f * b, back = b, side = 0.85f * b;
    const float frequency = std::clamp(0.5f * c.branch_flexibility, 0.0f, 5.0f), side_frequency = 0.8f * frequency;
    const float timer = tree_phase(c.form_id) + time;
    // Fewer bones further away, the highest first: by the fade metric of the tree's bound centre.
    const float scale = settings.fade_mult > 0.0f ? settings.lod_adjust / settings.fade_mult : 0.2f;
    const float metric = glm::distance(c.centre, camera) / k_unit_m * scale * (1.0f / 7200.0f);
    const size_t count = m.branches.size();
    const size_t shown = static_cast<size_t>(
        static_cast<float>(count) * std::clamp((0.14999998f - metric) * 10.000004f, 0.0f, 1.0f));
    for (size_t i = 0; i < shown; ++i) {
        const Branch& br = m.branches[i];
        const float along = wx * br.direction.x + wy * br.direction.y;
        const glm::vec3 bend_axis = br.to_parent * glm::vec3(-wy, wx, 0.0f);
        const glm::vec3 side_axis = br.to_parent * glm::vec3(0.0f, 0.0f, 1.0f);
        float amplitude = back;
        if (front != back) {
            const float u = std::clamp(((0.5f - 0.5f * along) - back) / (front - back), 0.0f, 1.0f);
            amplitude = std::min(std::max(u, front), back);
        }
        const float phase = (table_cos(static_cast<uint32_t>(i)) + timer) * settings.wave_scale;
        const glm::quat bend = angle_axis(branch_wave(phase * frequency) * amplitude, bend_axis);
        const glm::quat swing = angle_axis(branch_wave(phase * side_frequency) * side, side_axis);
        const glm::quat q = fast_slerp(swing, bend, std::abs(along));
        t.locals[size_t(br.node)] = glm::mat3_cast(q) * m.nodes[size_t(br.node)].rotation;
    }
    // The nodes in the model's frame, then each skinned bone's rows.
    std::vector<glm::mat4>& world = t.world;
    world.assign(m.nodes.size(), glm::mat4(1.0f));
    for (size_t i = 0; i < m.nodes.size(); ++i) {
        const Node& n = m.nodes[i];
        glm::mat4 local(t.locals[i] * n.scale);
        local[3] = glm::vec4(n.translation, 1.0f);
        world[i] = n.parent >= 0 ? world[size_t(n.parent)] * local : local;
    }
    const render::SkinnedModel& skinned = *m_meshes.skinned_model(m.mesh);
    for (size_t k = 0; k < skinned.shapes.size() && k < t.targets.size(); ++k) {
        if (t.targets[k] == 0) {
            continue;
        }
        const render::SkinnedShape& shape = skinned.shapes[k];
        render::SkinJob job;
        job.source = shape.vertices;
        job.skin = shape.skin;
        job.target = t.targets[k];
        job.vertices = shape.vertex_count;
        job.flags = shape.axes ? render::k_skin_axes : 0u;
        job.first_bone = static_cast<uint32_t>(rows.size() / 3);
        for (uint32_t bone = shape.first_bone; bone < shape.first_bone + shape.bone_count; ++bone) {
            const int32_t node = m.skin_nodes[bone];
            glm::mat4 r = rows_to_mat4(skinned.bones[bone].skin_to_bone);
            if (node >= 0) {
                r = world[size_t(node)] * r;
            }
            for (int row = 0; row < 3; ++row) {
                rows.emplace_back(r[0][row], r[1][row], r[2][row], r[3][row]);
            }
        }
        jobs.push_back(job);
    }
    t.placed = glm::translate(glm::mat4(1.0f), c.position) * glm::mat4_cast(c.rotation) *
               glm::scale(glm::mat4(1.0f), glm::vec3(c.scale));
    t.used = m_tick;
}

bool TreeSway::node(uint32_t form_id, std::string_view name, glm::vec3& position, glm::mat3& rotation) const {
    const auto it = m_trees.find(form_id);
    if (it == m_trees.end() || it->second.world.empty()) {
        return false;
    }
    const Model& m = m_models[it->second.model];
    for (size_t i = 0; i < m.names.size(); ++i) {
        if (m.names[i].size() == name.size() &&
            std::equal(name.begin(), name.end(), m.names[i].begin(),
                       [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); })) {
            const glm::mat4 w = it->second.placed * it->second.world[i];
            position = glm::vec3(w[3]);
            rotation = glm::mat3(w);
            for (int c = 0; c < 3; ++c) {
                rotation[c] = glm::normalize(rotation[c]);
            }
            return true;
        }
    }
    return false;
}

void TreeSway::update(std::span<Candidate> candidates, std::span<const uint32_t> loaded, const Settings& settings,
                      float time, const glm::vec3& camera, std::vector<render::SkinJob>& jobs,
                      std::vector<glm::vec4>& rows) {
    ++m_tick;
    // A tree no longer loaded, or too far to sway, goes back to its plain model.
    std::vector<uint32_t> keep(loaded.begin(), loaded.end());
    std::ranges::sort(keep);
    for (auto it = m_trees.begin(); it != m_trees.end();) {
        if (!std::ranges::binary_search(keep, it->first)) {
            free(it->second);
            it = m_trees.erase(it);
        } else {
            ++it;
        }
    }
    // The nearest in view, at most the setting's.
    const size_t take = std::min<size_t>(candidates.size(), settings.max_trees);
    std::ranges::partial_sort(candidates, candidates.begin() + std::ptrdiff_t(take), {}, &Candidate::distance_sq);
    for (size_t i = 0; i < take; ++i) {
        const Candidate& c = candidates[i];
        uint32_t index = 0;
        Model* m = model(*c.mesh, index);
        if (!prepare(*m)) {
            continue;
        }
        if (Tree* t = tree(c, index)) {
            pose(*t, *m, c, settings, time, camera, jobs, rows);
        }
    }
    // Copies the game would keep, unposed, beyond what memory wants: the longest unposed go first.
    const size_t most = size_t(settings.max_trees) * 2;
    while (m_trees.size() > most) {
        auto oldest = m_trees.begin();
        for (auto it = m_trees.begin(); it != m_trees.end(); ++it) {
            if (it->second.used < oldest->second.used) {
                oldest = it;
            }
        }
        free(oldest->second);
        m_trees.erase(oldest);
    }
}

bool TreeSway::drawn_as(uint32_t form_id, render::MeshId& mesh, uint32_t& variant) const {
    const auto it = m_trees.find(form_id);
    if (it == m_trees.end() || it->second.variant == render::k_no_variant) {
        return false;
    }
    mesh = m_models[it->second.model].mesh;
    variant = it->second.variant;
    return true;
}

} // namespace spud
