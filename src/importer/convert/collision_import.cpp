#include "importer/convert/collision_import.hpp"

#include "core/units.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <map>
#include <span>
#include <unordered_map>

namespace spud::importer {

using namespace spud::legacy;

namespace {

constexpr int k_max_depth = 256;
constexpr size_t k_max_warnings = 32;
// Mesh vertices closer than this are welded, so triangles of neighbouring compressed-mesh chunks share
// edges and the character slides over the seams. Well below the quantisation step (1 mm).
constexpr float k_weld_m = 0.0002f;

// x -> parent space: parent = linear * x + translation.
struct Affine {
    glm::mat3 linear{1.0f};
    glm::vec3 translation{0.0f};

    glm::vec3 point(const glm::vec3& p) const { return linear * p + translation; }
    Affine then(const Affine& child) const {
        return {linear * child.linear, linear * child.translation + translation};
    }
};

glm::mat3 quat_matrix(const glm::vec4& xyzw) {
    glm::quat q(xyzw.w, xyzw.x, xyzw.y, xyzw.z);
    const float len = glm::length(q);
    if (!(len > 1e-6f) || !std::isfinite(len)) {
        return glm::mat3(1.0f);
    }
    return glm::mat3_cast(q / len);
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

struct WeldKey {
    int64_t x, y, z;
    bool operator==(const WeldKey&) const = default;
};
struct WeldHash {
    size_t operator()(const WeldKey& k) const {
        return std::hash<int64_t>()(k.x * 73856093 ^ k.y * 19349663 ^ k.z * 83492791);
    }
};

// Triangles of one (layer, material) group, welded as they are added.
// Where Havok's MOPP walk visits a shape key: depth first, the first
// child before the second, so the order of any two keys is fixed by the code whatever box is queried. Returns the
// keys in that order, or nothing when the code doesn't decode.
std::vector<uint32_t> mopp_leaf_order(std::span<const uint8_t> code) {
    std::vector<uint32_t> keys;
    if (code.empty()) {
        return keys;
    }
    const bool chunk_bits = code[0] == 0x0D;
    struct Branch {
        size_t pos = 0;
        uint32_t offset = 0; // added to terminal keys
        uint32_t chunk = 0;  // byte offset of the chunk last jumped to
    };
    std::vector<Branch> pending{{}};
    size_t steps = 0;
    while (!pending.empty()) {
        Branch b = pending.back();
        pending.pop_back();
        for (;;) {
            if (b.pos >= code.size() || ++steps > 100'000'000) {
                return {};
            }
            auto u = [&](size_t i) -> uint32_t { return b.pos + i < code.size() ? code[b.pos + i] : 0u; };
            const uint8_t op = code[b.pos];
            auto emit = [&](uint32_t value) {
                uint32_t key = value + b.offset;
                if (chunk_bits) {
                    key |= (b.chunk >> 9) << 8;
                }
                keys.push_back(key);
            };
            if (op == 0x00) {
                break;
            } else if (op >= 0x01 && op <= 0x04) {
                b.pos += 4; // rescale: only the bounds change
            } else if (op == 0x05) {
                b.pos += u(1) + 2;
            } else if (op == 0x06) {
                b.pos += (u(1) << 8 | u(2)) + 3;
            } else if (op == 0x07) {
                b.pos += (u(1) << 16 | u(2) << 8 | u(3)) + 4;
            } else if (op == 0x09) {
                b.offset += u(1);
                b.pos += 2;
            } else if (op == 0x0A) {
                b.offset += u(1) << 8 | u(2);
                b.pos += 3;
            } else if (op == 0x0B) {
                b.offset = u(1) << 24 | u(2) << 16 | u(3) << 8 | u(4);
                b.pos += 5;
            } else if (op == 0x0C) {
                b.chunk = (u(1) << 8 | u(2)) << 9;
                b.pos = b.chunk;
            } else if (op == 0x70) {
                b.chunk = u(1) << 24 | u(2) << 16 | u(3) << 8 | u(4);
                b.pos = b.chunk;
            } else if (op == 0x0D) {
                b.pos += 5;
            } else if (op >= 0x10 && op <= 0x1C) {
                Branch second = b;
                second.pos = b.pos + 4 + u(3);
                pending.push_back(second);
                b.pos += 4;
            } else if (op >= 0x20 && op <= 0x22) {
                Branch second = b;
                second.pos = b.pos + 3 + u(2);
                pending.push_back(second);
                b.pos += 3;
            } else if (op >= 0x23 && op <= 0x25) {
                const size_t end = b.pos + 7;
                Branch second = b;
                second.pos = end + (u(5) << 8 | u(6));
                pending.push_back(second);
                b.pos = end + (u(3) << 8 | u(4));
            } else if (op >= 0x26 && op <= 0x28) {
                b.pos += 3;
            } else if (op >= 0x29 && op <= 0x2B) {
                b.pos += 7;
            } else if (op >= 0x30 && op <= 0x4F) {
                emit(op - 0x30u);
                break;
            } else if (op == 0x50) {
                emit(u(1));
                break;
            } else if (op == 0x51) {
                emit(u(1) << 8 | u(2));
                break;
            } else if (op == 0x52) {
                emit(u(1) << 16 | u(2) << 8 | u(3));
                break;
            } else if (op == 0x53) {
                emit(u(1) << 24 | u(2) << 16 | u(3) << 8 | u(4));
                break;
            } else if (op >= 0x60 && op <= 0x63) {
                b.pos += 2;
            } else if (op >= 0x64 && op <= 0x67) {
                b.pos += 3;
            } else if (op >= 0x68 && op <= 0x6B) {
                b.pos += 5;
            } else {
                return {};
            }
        }
    }
    return keys;
}

constexpr uint32_t k_no_rank = 0xFFFFFFFFu;

struct MeshGroup {
    uint16_t body = 0;
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> ranks; // per triangle: Havok's collection order (osp RANK)
    std::unordered_map<WeldKey, uint32_t, WeldHash> lookup;

    uint32_t vertex(const glm::vec3& p) {
        const WeldKey k{std::llround(p.x / k_weld_m), std::llround(p.y / k_weld_m), std::llround(p.z / k_weld_m)};
        auto [it, inserted] = lookup.emplace(k, static_cast<uint32_t>(vertices.size()));
        if (inserted) {
            vertices.push_back(p);
        }
        return it->second;
    }

    void triangle(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, uint32_t rank = k_no_rank) {
        const glm::vec3 n = glm::cross(b - a, c - a);
        if (!(glm::dot(n, n) > 1e-14f) || !std::isfinite(n.x + n.y + n.z)) {
            return; // degenerate
        }
        const uint32_t ia = vertex(a), ib = vertex(b), ic = vertex(c);
        if (ia == ib || ib == ic || ia == ic) {
            return;
        }
        indices.insert(indices.end(), {ia, ib, ic});
        ranks.push_back(rank);
    }
};

class CollisionBuilder {
public:
    CollisionBuilder(const NifFile& nif, CollisionImportResult& out, int damage_stage,
                     const std::unordered_map<std::string, uint32_t>* nodes)
        : m_nif(nif), m_out(out), m_on_path(nif.blocks.size(), 0), m_unit_to_m(units::k_skyrim_unit_m),
          m_havok_to_units(k_havok_scale), m_damage_stage(damage_stage), m_nodes(nodes) {}

    void visit(NifRef ref, const Affine& parent, int depth) {
        if (!m_nif.valid(ref)) {
            return;
        }
        const auto index = static_cast<size_t>(ref);
        const NiAVObject* av = m_nif.av_object(ref);
        if (!av || starts_with_ci(av->name, "EditorMarker")) {
            return;
        }
        if (m_on_path[index] || depth > k_max_depth) {
            return;
        }
        const auto type = m_nif.type_name(ref);
        const auto* node = m_nif.get<NiNode>(ref);
        if (type == "BSDamageStage" && (!node || m_damage_stage < node->range[0] || m_damage_stage > node->range[1])) {
            return; // only the stage shown collides
        }
        // Collision ignores the APP_CULLED flag: hidden nodes still collide in the game.
        // The reference's transform takes the place of the root's own, as it does for what draws (mesh_import.cpp). Whiterun's
        // ground models keep the rotation they were placed with on their root: applied here, their collision is turned twice.
        const Affine world = depth == 0 ? parent : parent.then({av->rotation * av->scale, av->translation});
        if (av->collision != -1) {
            if (m_nodes) {
                const auto at = m_nodes->find(av->name);
                m_node = at != m_nodes->end() ? static_cast<uint16_t>(at->second + 1) : 0;
            }
            collision_object(av->collision, m_nodes ? Affine{} : world);
        }
        if (node) {
            m_on_path[index] = 1;
            if (node->switch_index) {
                if (*node->switch_index < node->children.size()) {
                    visit(node->children[*node->switch_index], world, depth + 1);
                }
            } else {
                for (auto c : node->children) {
                    visit(c, world, depth + 1);
                }
            }
            m_on_path[index] = 0;
        }
    }

    void finish() {
        auto& c = m_out.collision;
        for (auto& [key, group] : m_groups) {
            if (group.indices.empty()) {
                continue;
            }
            osp::Shape s;
            s.type = osp::ShapeType::mesh;
            s.layer = key.layer;
            s.material = key.material;
            s.convex_radius = float(key.radius_um) * 1e-6f;
            s.body = group.body;
            s.first_vertex = static_cast<uint32_t>(c.vertices.size() / 3);
            s.vertex_count = static_cast<uint32_t>(group.vertices.size());
            s.first_index = static_cast<uint32_t>(c.indices.size());
            s.index_count = static_cast<uint32_t>(group.indices.size());
            for (const auto& v : group.vertices) {
                c.vertices.insert(c.vertices.end(), {v.x, v.y, v.z});
            }
            c.indices.insert(c.indices.end(), group.indices.begin(), group.indices.end());
            c.ranks.insert(c.ranks.end(), group.ranks.begin(), group.ranks.end());
            c.shapes.push_back(s);
        }
        m_groups.clear();
        if (c.shapes.empty()) {
            c.bodies.clear();
            c.vertices.clear();
            c.indices.clear();
            c.ranks.clear();
        }
        if (std::ranges::all_of(c.ranks, [](uint32_t r) { return r == k_no_rank; })) {
            c.ranks.clear(); // no MOPP in this file
        }
        osp::compute_bounds(c);
    }

private:
    struct GroupKey {
        uint16_t body;
        uint8_t layer;
        uint32_t material;
        uint32_t radius_um; // the triangles' Havok convex radius, micrometres
        auto operator<=>(const GroupKey&) const = default;
    };

    void warn(std::string message) {
        if (m_out.warnings.size() >= k_max_warnings ||
            std::ranges::find(m_out.warnings, message) != m_out.warnings.end()) {
            return;
        }
        m_out.warnings.push_back(std::move(message));
    }

    template <typename T>
    const T* get(NifRef ref, std::string_view what) {
        if (const T* t = m_nif.get<T>(ref)) {
            return t;
        }
        if (m_nif.valid(ref) && m_nif.blocks[static_cast<size_t>(ref)].status == NifBlockStatus::mismatch) {
            warn(std::format("{} block {} ({}) could not be decoded: {}", what, ref, m_nif.type_name(ref),
                             m_nif.blocks[static_cast<size_t>(ref)].error));
        } else if (ref != -1) {
            warn(std::format("{} block {} has unsupported type {}", what, ref, m_nif.type_name(ref)));
        }
        return nullptr;
    }

    void collision_object(NifRef ref, const Affine& node_world_units) {
        const auto type = m_nif.type_name(ref);
        if (type == "bhkSPCollisionObject" || type == "bhkPCollisionObject" || type == "bhkBlendCollisionObject") {
            return; // phantoms (triggers, bumpers) and ragdoll bones are not solid world collision
        }
        const auto* co = get<bhkCollisionObject>(ref, "collision object");
        if (!co) {
            return;
        }
        const auto* rb = get<bhkRigidBody>(co->body, "rigid body");
        if (!rb) {
            return;
        }
        // Havok units -> mesh metres.
        Affine base{node_world_units.linear * (m_havok_to_units * m_unit_to_m),
                    node_world_units.translation * m_unit_to_m};
        if (m_nif.type_name(co->body) == "bhkRigidBodyT") {
            base = base.then({quat_matrix(rb->rotation), glm::vec3(rb->translation)});
        }
        osp::Body body;
        body.mass = rb->mass;
        body.friction = rb->friction;
        body.restitution = rb->restitution;
        body.layer = rb->filter.layer;
        body.node = m_node;
        if (rb->motion_system == 6 || rb->filter.layer == osp::layer_anim_static) {
            body.motion = osp::Motion::keyframed;
        } else if (rb->mass > 0.0f && rb->motion_system != 7) {
            body.motion = osp::Motion::dynamic;
        }
        auto& bodies = m_out.collision.bodies;
        if (bodies.size() >= 0xFFFF) {
            warn("too many collision bodies");
            return;
        }
        m_body = static_cast<uint16_t>(bodies.size());
        bodies.push_back(body);
        shape(rb->shape, base, rb->filter.layer, 0);
    }

    // Shapes are placed with `xf` (Havok units -> mesh metres); `layer` is inherited from the parent.
    void shape(NifRef ref, const Affine& xf, uint8_t layer, int depth) {
        if (depth > 32 || !m_nif.valid(ref)) {
            return;
        }
        const auto type = m_nif.type_name(ref);
        const auto& data = m_nif.blocks[static_cast<size_t>(ref)].data;
        if (const auto* s = std::get_if<bhkMoppBvTreeShape>(&data)) {
            Affine child = xf;
            if (s->shape_scale != 1.0f && s->shape_scale > 0.0f) {
                child.linear *= s->shape_scale;
            }
            // Each MOPP's keys rank after the previous one's: a list shape's children are visited in order.
            m_mopp_rank.clear();
            const std::vector<uint32_t> order = mopp_leaf_order(s->code);
            if (order.empty() && !s->code.empty()) {
                warn(std::format("MOPP code of block {} doesn't decode; its triangles keep no collection order", ref));
            }
            for (size_t i = 0; i < order.size() && i < (1u << 20); ++i) {
                m_mopp_rank.emplace(order[i], m_mopp_base | static_cast<uint32_t>(i));
            }
            m_mopp_base += 1u << 20;
            shape(s->shape, child, layer, depth + 1);
            m_mopp_rank.clear();
            return;
        }
        if (const auto* s = std::get_if<bhkCompressedMeshShape>(&data)) {
            compressed_mesh(*s, xf, layer);
            return;
        }
        if (const auto* s = std::get_if<bhkListShape>(&data)) {
            for (size_t i = 0; i < s->shapes.size(); ++i) {
                const uint8_t child_layer = i < s->filters.size() && s->filters[i].layer != 0 ? s->filters[i].layer : layer;
                shape(s->shapes[i], xf, child_layer, depth + 1);
            }
            return;
        }
        if (const auto* s = std::get_if<bhkTransformShape>(&data)) {
            shape(s->shape, xf.then({glm::mat3(s->transform), glm::vec3(s->transform[3])}), layer, depth + 1);
            return;
        }
        if (const auto* s = std::get_if<bhkBoxShape>(&data)) {
            box(*s, xf, layer);
            return;
        }
        if (const auto* s = std::get_if<bhkSphereShape>(&data)) {
            osp::Shape out = primitive(osp::ShapeType::sphere, s->material, layer);
            set(out.centre, xf.point(glm::vec3(0.0f)));
            out.radius = s->radius * scale_of(xf);
            m_out.collision.shapes.push_back(out);
            return;
        }
        if (const auto* s = std::get_if<bhkCapsuleShape>(&data)) {
            osp::Shape out = primitive(osp::ShapeType::capsule, s->material, layer);
            set(out.centre, xf.point(s->a));
            set(out.extents, xf.point(s->b));
            out.radius = s->radius * scale_of(xf);
            m_out.collision.shapes.push_back(out);
            return;
        }
        if (const auto* s = std::get_if<bhkCylinderShape>(&data)) {
            osp::Shape out = primitive(osp::ShapeType::cylinder, s->material, layer);
            set(out.centre, xf.point(s->a));
            set(out.extents, xf.point(s->b));
            out.radius = s->cylinder_radius * scale_of(xf);
            out.convex_radius = s->radius * scale_of(xf);
            m_out.collision.shapes.push_back(out);
            return;
        }
        if (const auto* s = std::get_if<bhkConvexVerticesShape>(&data)) {
            std::vector<glm::vec3> points;
            points.reserve(s->vertices.size());
            for (const auto& v : s->vertices) {
                points.push_back(xf.point(glm::vec3(v)));
            }
            convex(points, s->material, layer, s->radius * scale_of(xf));
            return;
        }
        if (const auto* s = std::get_if<bhkPackedNiTriStripsShape>(&data)) {
            packed_strips(*s, xf, layer);
            return;
        }
        if (const auto* s = std::get_if<bhkNiTriStripsShape>(&data)) {
            for (size_t i = 0; i < s->strips.size(); ++i) {
                const auto* g = get<NiGeometryData>(s->strips[i], "collision strips");
                if (!g) {
                    continue;
                }
                const uint8_t l = i < s->filters.size() && s->filters[i].layer != 0 ? s->filters[i].layer : layer;
                // Oblivion-style strips are in Skyrim units, not Havok units.
                Affine units_xf{xf.linear / m_havok_to_units, xf.translation};
                auto& group = mesh_group(l, s->material, s->radius * scale_of(xf));
                const auto& p = g->geometry.positions;
                for (const auto& t : g->geometry.triangles) {
                    if (t[0] < p.size() && t[1] < p.size() && t[2] < p.size()) {
                        group.triangle(units_xf.point(p[t[0]]), units_xf.point(p[t[1]]), units_xf.point(p[t[2]]));
                    }
                }
            }
            return;
        }
        if (m_nif.blocks[static_cast<size_t>(ref)].status == NifBlockStatus::mismatch) {
            warn(std::format("shape block {} ({}) could not be decoded: {}", ref, type,
                             m_nif.blocks[static_cast<size_t>(ref)].error));
        } else {
            warn(std::format("unsupported collision shape {} (block {})", type, ref));
        }
    }

    osp::Shape primitive(osp::ShapeType type, uint32_t material, uint8_t layer) const {
        osp::Shape s;
        s.type = type;
        s.material = material;
        s.layer = layer;
        s.body = m_body;
        return s;
    }

    static void set(float* dst, const glm::vec3& v) {
        dst[0] = v.x;
        dst[1] = v.y;
        dst[2] = v.z;
    }

    // Uniform scale of a transform (mean column length).
    static float scale_of(const Affine& xf) {
        return (glm::length(xf.linear[0]) + glm::length(xf.linear[1]) + glm::length(xf.linear[2])) / 3.0f;
    }

    void box(const bhkBoxShape& s, const Affine& xf, uint8_t layer) {
        glm::mat3 m = xf.linear;
        glm::vec3 len(glm::length(m[0]), glm::length(m[1]), glm::length(m[2]));
        const bool orthogonal = len.x > 1e-8f && len.y > 1e-8f && len.z > 1e-8f &&
                                std::abs(glm::dot(m[0], m[1])) < 1e-4f * len.x * len.y &&
                                std::abs(glm::dot(m[1], m[2])) < 1e-4f * len.y * len.z &&
                                std::abs(glm::dot(m[0], m[2])) < 1e-4f * len.x * len.z;
        if (!orthogonal) {
            // Sheared by the node transforms: keep its exact corners as a hull.
            std::vector<glm::vec3> corners;
            for (int i = 0; i < 8; ++i) {
                const glm::vec3 c((i & 1) ? s.half_extents.x : -s.half_extents.x,
                                  (i & 2) ? s.half_extents.y : -s.half_extents.y,
                                  (i & 4) ? s.half_extents.z : -s.half_extents.z);
                corners.push_back(xf.point(c));
            }
            convex(corners, s.material, layer, s.radius * scale_of(xf));
            return;
        }
        glm::mat3 r(m[0] / len.x, m[1] / len.y, m[2] / len.z);
        if (glm::determinant(r) < 0.0f) {
            r[0] = -r[0]; // a mirrored box is the same box
        }
        osp::Shape out = primitive(osp::ShapeType::box, s.material, layer);
        set(out.centre, xf.translation);
        const glm::quat q = glm::normalize(glm::quat_cast(r));
        out.rotation[0] = q.x;
        out.rotation[1] = q.y;
        out.rotation[2] = q.z;
        out.rotation[3] = q.w;
        set(out.extents, s.half_extents * len);
        out.convex_radius = s.radius * scale_of(xf);
        m_out.collision.shapes.push_back(out);
    }

    void convex(const std::vector<glm::vec3>& points, uint32_t material, uint8_t layer, float radius) {
        if (points.size() < 4) {
            warn(std::format("convex shape with {} points dropped", points.size()));
            return;
        }
        auto& c = m_out.collision;
        osp::Shape out = primitive(osp::ShapeType::convex, material, layer);
        out.convex_radius = radius;
        out.first_vertex = static_cast<uint32_t>(c.vertices.size() / 3);
        out.vertex_count = static_cast<uint32_t>(points.size());
        for (const auto& p : points) {
            c.vertices.insert(c.vertices.end(), {p.x, p.y, p.z});
        }
        c.shapes.push_back(out);
    }

    MeshGroup& mesh_group(uint8_t layer, uint32_t material, float radius) {
        const auto um = static_cast<uint32_t>(std::lround(std::max(radius, 0.0f) * 1e6f));
        auto& g = m_groups[GroupKey{m_body, layer, material, um}];
        g.body = m_body;
        return g;
    }

    void compressed_mesh(const bhkCompressedMeshShape& s, const Affine& xf, uint8_t body_layer) {
        const auto* d = get<bhkCompressedMeshShapeData>(s.data, "compressed mesh data");
        if (!d) {
            return;
        }
        Affine shape_xf = xf;
        if (s.scale.x != 1.0f || s.scale.y != 1.0f || s.scale.z != 1.0f) {
            if (s.scale.x > 0.0f && s.scale.y > 0.0f && s.scale.z > 0.0f) {
                glm::mat3 scale(1.0f);
                scale[0][0] = s.scale.x;
                scale[1][1] = s.scale.y;
                scale[2][2] = s.scale.z;
                shape_xf.linear = xf.linear * scale;
            }
        }
        auto material = [&](uint32_t i, uint32_t& id, uint8_t& layer) {
            if (i < d->materials.size()) {
                id = d->materials[i].material;
                layer = d->materials[i].filter.layer != 0 ? d->materials[i].filter.layer : body_layer;
            } else {
                id = 0;
                layer = body_layer;
            }
        };
        size_t bad = 0, unranked = 0;
        // A triangle's shape key and its place in the MOPP's order.
        auto rank_of = [&](uint32_t key) {
            if (m_mopp_rank.empty()) {
                return k_no_rank;
            }
            const auto it = m_mopp_rank.find(key);
            if (it == m_mopp_rank.end()) {
                ++unranked;
                return k_no_rank;
            }
            return it->second;
        };
        std::vector<glm::vec3> verts;
        for (size_t ci = 0; ci < d->chunks.size(); ++ci) {
            const auto& chunk = d->chunks[ci];
            const uint32_t chunk_key = static_cast<uint32_t>(ci + 1) << d->bits_per_w_index;
            Affine chunk_xf = shape_xf;
            if (chunk.transform < d->transforms.size()) {
                const auto& t = d->transforms[chunk.transform];
                chunk_xf = shape_xf.then({quat_matrix(t.rotation), glm::vec3(t.translation)});
            } else if (chunk.transform != 0xFFFF && !d->transforms.empty()) {
                ++bad;
            }
            const size_t n = chunk.vertices.size() / 3;
            verts.resize(n);
            const glm::vec3 origin(chunk.translation);
            for (size_t i = 0; i < n; ++i) {
                const glm::vec3 q(chunk.vertices[i * 3], chunk.vertices[i * 3 + 1], chunk.vertices[i * 3 + 2]);
                verts[i] = chunk_xf.point(origin + q * d->error);
            }
            uint32_t id;
            uint8_t layer;
            material(chunk.material, id, layer);
            auto& group = mesh_group(layer, id, s.radius * scale_of(xf));
            auto tri = [&](uint16_t a, uint16_t b, uint16_t c, uint32_t key) {
                if (a >= n || b >= n || c >= n) {
                    ++bad;
                    return;
                }
                group.triangle(verts[a], verts[b], verts[c], rank_of(key));
            };
            size_t at = 0;
            for (uint16_t len : chunk.strips) {
                if (at + len > chunk.indices.size()) {
                    ++bad;
                    break;
                }
                for (size_t i = 2; i < len; ++i) {
                    const size_t first = at + i - 2;
                    const uint16_t* p = &chunk.indices[first];
                    // The key: the chunk, the winding bit (set on a strip's odd triangles), the first index's place.
                    const uint32_t key = chunk_key | (static_cast<uint32_t>(i % 2) << d->bits_per_index) |
                                         static_cast<uint32_t>(first);
                    if (i % 2 == 0) {
                        tri(p[0], p[1], p[2], key);
                    } else {
                        tri(p[1], p[0], p[2], key); // strips alternate winding
                    }
                }
                at += len;
            }
            for (; at + 2 < chunk.indices.size(); at += 3) {
                tri(chunk.indices[at], chunk.indices[at + 1], chunk.indices[at + 2], chunk_key | static_cast<uint32_t>(at));
            }
        }
        const size_t nb = d->big_vertices.size();
        for (size_t bi = 0; bi < d->big_triangles.size(); ++bi) {
            const auto& t = d->big_triangles[bi];
            if (t.vertices[0] >= nb || t.vertices[1] >= nb || t.vertices[2] >= nb) {
                ++bad;
                continue;
            }
            uint32_t id;
            uint8_t layer;
            material(t.material, id, layer);
            mesh_group(layer, id, s.radius * scale_of(xf))
                .triangle(shape_xf.point(glm::vec3(d->big_vertices[t.vertices[0]])),
                          shape_xf.point(glm::vec3(d->big_vertices[t.vertices[1]])),
                          shape_xf.point(glm::vec3(d->big_vertices[t.vertices[2]])), rank_of(static_cast<uint32_t>(bi)));
        }
        if (bad > 0) {
            warn(std::format("compressed mesh: {} bad triangles or chunk transforms", bad));
        }
        if (unranked > 0) {
            warn(std::format("compressed mesh: {} triangles' keys are not in the MOPP code", unranked));
        }
    }

    void packed_strips(const bhkPackedNiTriStripsShape& s, const Affine& xf, uint8_t body_layer) {
        const auto* d = get<hkPackedNiTriStripsData>(s.data, "packed strips data");
        if (!d) {
            return;
        }
        // Sub shapes own consecutive runs of vertices; a triangle belongs to the run of its first vertex.
        std::vector<std::pair<uint32_t, size_t>> runs; // (end vertex, sub shape)
        uint32_t end = 0;
        for (size_t i = 0; i < d->sub_shapes.size(); ++i) {
            end += d->sub_shapes[i].vertex_count;
            runs.emplace_back(end, i);
        }
        const size_t n = d->vertices.size();
        size_t bad = 0;
        for (const auto& t : d->triangles) {
            if (t[0] >= n || t[1] >= n || t[2] >= n) {
                ++bad;
                continue;
            }
            uint32_t id = 0;
            uint8_t layer = body_layer;
            auto it = std::ranges::upper_bound(runs, t[0], {}, &std::pair<uint32_t, size_t>::first);
            if (it != runs.end()) {
                const auto& sub = d->sub_shapes[it->second];
                id = sub.material;
                layer = sub.filter.layer != 0 ? sub.filter.layer : body_layer;
            }
            mesh_group(layer, id, s.radius * scale_of(xf)).triangle(xf.point(d->vertices[t[0]]), xf.point(d->vertices[t[1]]),
                                           xf.point(d->vertices[t[2]]));
        }
        if (bad > 0) {
            warn(std::format("packed strips: {} triangles reference missing vertices", bad));
        }
    }

    const NifFile& m_nif;
    int m_damage_stage = 0;
    const std::unordered_map<std::string, uint32_t>* m_nodes = nullptr;
    uint16_t m_node = 0;
    CollisionImportResult& m_out;
    std::vector<uint8_t> m_on_path;
    const float m_unit_to_m;
    const float m_havok_to_units;
    uint16_t m_body = 0;
    std::map<GroupKey, MeshGroup> m_groups;
    std::unordered_map<uint32_t, uint32_t> m_mopp_rank; // the MOPP being imported: shape key -> rank
    uint32_t m_mopp_base = 0;                            // ranks of the next MOPP start here
};

} // namespace

CollisionImportResult import_collision(const NifFile& nif, std::string_view /*source_path*/, int damage_stage,
                                       const std::unordered_map<std::string, uint32_t>* nodes) {
    CollisionImportResult result;
    CollisionBuilder builder(nif, result, damage_stage, nodes);
    for (auto root : nif.roots) {
        builder.visit(root, Affine{}, 0);
    }
    builder.finish();
    return result;
}

} // namespace spud::importer
