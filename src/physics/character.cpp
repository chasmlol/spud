#include "physics/jolt.hpp"
#include "physics/simplex.hpp"

#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/TransformedShape.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spud::physics {

// Skyrim's character controller as the game runs it for the player (
// "Character proxy"): the controller's states set a velocity each step, and a Havok character proxy moves the
// hull through the world with its manifold of nearby contacts, the game's constraint callback (walkable
// ground, steps, steep walls) and Havok's simplex solver. Jolt only answers the collision queries: the contacts
// within reach of the hull and the hull swept along a displacement. Distances are Havok units (metres here).
//
// The hull's surface is Havok's: its vertices grown by the convex radius. Jolt sees the bare vertex hull, so
// every distance below takes the convex radius off.

namespace {

constexpr float k_eps = 1.1920929e-7f;
constexpr float k_keep_distance = 0.05f;        // keepDistance
constexpr float k_keep_tolerance = 0.1f;        // keepContactTolerance
constexpr float k_penetration_recovery = 1.0f;  // penetrationRecoverySpeed, 1/s
constexpr int k_max_cast_iterations = 10;       // maxCastIterations
constexpr float k_extra_down_still = 5.77f;     // walkable ground under a character not moving
constexpr float k_extra_down_animstatic = 0.577f;
constexpr float k_steep_support = 0.2f;         // steep contacts still hold the character above this normal z
constexpr int k_max_walls = 4;                  // userPlanes
constexpr float k_fast = 500.0f;                // |v|^2 above which steps are left alone
constexpr float k_step_capsule = 0.05f;
constexpr uint8_t k_layer_animstatic = 2;
constexpr uint8_t k_layer_clutter = 4, k_layer_weapon = 5, k_layer_projectile = 6, k_layer_props = 10;
constexpr float k_move_limit_mass = 95.0f; // fMoveLimitMass:HAVOK
constexpr uint8_t k_layer_trap = 14;
constexpr float k_unit = 0.0142875f; // metres (Havok units) per Skyrim unit
constexpr float k_max_shape_radius = 0.1f;      // the largest shell a world shape gets, for the query reach
// Linear casts (hkpLinearCastInput as integrate fills it; hkpWorldCinfo's agent config, left at Havok's defaults).
constexpr float k_max_extra_penetration = 0.01f; // maxExtraPenetration
constexpr float k_cast_early_out = 0.01f;        // iterativeLinearCastEarlyOutDistance
constexpr int k_cast_iterations = 20;            // iterativeLinearCastMaxIterations

// A root contact point of the proxy's manifold.
struct Contact {
    glm::vec3 position{0}; // on the surface
    glm::vec3 normal{0, 0, 1}; // separating normal, toward the character
    float distance = 0;    // from the hull's surface
    JPH::BodyID body;
    JPH::SubShapeID sub;
    uint8_t layer = 0;
    uint32_t material = 0;
    int tag = 0;           // triangles: 1 face contact, 2 edge or vertex; 0 other shapes
    float fraction = 0;    // cast hits: along the sweep
    bool dynamic = false;  // a simulated body (clutter)
    float mass = 0;
    bool keyframed = false; // an animated object
    glm::vec3 velocity{0};  // the surface's here, on one of those
};

// Havok's surface distance between two contacts (point velocities are zero for the static world).
float surface_distance(const Contact& a, const Contact& b) {
    const float angle = 10.0f * 10.0f * 10.0f * (1.0f - glm::dot(a.normal, b.normal)); // 10 x sensitivity^2
    const float dd = a.distance - b.distance;
    return angle + dd * dd;
}

bool is_triangle(const JPH::Shape* leaf) {
    const auto t = leaf ? leaf->GetSubType() : JPH::EShapeSubType::Sphere;
    return t == JPH::EShapeSubType::Mesh || t == JPH::EShapeSubType::HeightField;
}

// Squared distance between segment [p, q] and triangle abc.
float segment_triangle_distance2(glm::vec3 p, glm::vec3 q, glm::vec3 a, glm::vec3 b, glm::vec3 c) {
    auto point_triangle = [&](glm::vec3 x) {
        // Closest point on the triangle (Ericson, Real-Time Collision Detection 5.1.5).
        const glm::vec3 ab = b - a, ac = c - a, ax = x - a;
        const float d1 = glm::dot(ab, ax), d2 = glm::dot(ac, ax);
        if (d1 <= 0 && d2 <= 0) return a;
        const glm::vec3 bx = x - b;
        const float d3 = glm::dot(ab, bx), d4 = glm::dot(ac, bx);
        if (d3 >= 0 && d4 <= d3) return b;
        const float vc = d1 * d4 - d3 * d2;
        if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
        const glm::vec3 cx = x - c;
        const float d5 = glm::dot(ab, cx), d6 = glm::dot(ac, cx);
        if (d6 >= 0 && d5 <= d6) return c;
        const float vb = d5 * d2 - d1 * d6;
        if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
        const float va = d3 * d6 - d5 * d4;
        if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
        const float denom = 1.0f / (va + vb + vc);
        return a + ab * (vb * denom) + ac * (vc * denom);
    };
    auto segment_segment = [](glm::vec3 p1, glm::vec3 q1, glm::vec3 p2, glm::vec3 q2) {
        const glm::vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
        const float a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
        float s = 0, t = 0;
        if (a <= k_eps && e <= k_eps) {
            return glm::dot(r, r);
        }
        if (a <= k_eps) {
            t = std::clamp(f / e, 0.0f, 1.0f);
        } else {
            const float c = glm::dot(d1, r);
            if (e <= k_eps) {
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else {
                const float b = glm::dot(d1, d2), den = a * e - b * b;
                s = den != 0 ? std::clamp((b * f - c * e) / den, 0.0f, 1.0f) : 0.0f;
                t = (b * s + f) / e;
                if (t < 0) {
                    t = 0;
                    s = std::clamp(-c / a, 0.0f, 1.0f);
                } else if (t > 1) {
                    t = 1;
                    s = std::clamp((b - c) / a, 0.0f, 1.0f);
                }
            }
        }
        const glm::vec3 d = (p1 + d1 * s) - (p2 + d2 * t);
        return glm::dot(d, d);
    };
    // The segment crossing the triangle's plane inside it.
    const glm::vec3 n = glm::cross(b - a, c - a);
    const float dp = glm::dot(n, p - a), dq = glm::dot(n, q - a);
    if (dp * dq <= 0 && dp != dq) {
        const glm::vec3 x = p + (q - p) * (dp / (dp - dq));
        const glm::vec3 y = point_triangle(x);
        if (glm::dot(x - y, x - y) < 1e-10f) {
            return 0.0f;
        }
    }
    float best = std::numeric_limits<float>::max();
    for (const glm::vec3 x : {p, q}) {
        const glm::vec3 y = point_triangle(x);
        best = std::min(best, glm::dot(x - y, x - y));
    }
    best = std::min({best, segment_segment(p, q, a, b), segment_segment(p, q, b, c), segment_segment(p, q, c, a)});
    return best;
}

} // namespace

struct Character::Impl {
    World& world;
    JPH::PhysicsSystem& system;
    JPH::RefConst<JPH::Shape> hull; // the vertex hull, centred `centre` above the feet
    float centre = 0;
    float convex_radius = 0;
    float radius = 0;      // r, the hull's horizontal radius
    float step = 0;        // S
    float probe = 0;       // step the game distance
    float height = 0;      // the controller height (2 hz)
    float cos_walkable = 0;
    std::vector<uint32_t> stairs_materials; // sorted
    JPH::TempAllocatorImpl temp{1u << 20};

    // The proxy.
    glm::vec3 feet{0};
    float turn = 0; // the hull's rotation about +Z (radians, counter-clockwise): local +y along the facing
    glm::vec3 velocity{0};          // the proxy velocity: the solver's last output
    glm::vec3 requested_velocity{0}; // what the state asked of the proxy this step
    glm::vec3 old_displacement{0};
    std::vector<Contact> manifold;

    // The controller.
    ControllerState state = ControllerState::on_ground;
    ControllerState previous_state = ControllerState::on_ground;
    bool jump_pending = false; // a jump committed on the last step takes off on this one
    bool take_off = false;     // the jump take-off window (flag 0x2000)
    float fall_start = 0;
    float air_time = 0;
    bool landed = false;
    float fall_distance = 0;
    bool swimming = false;
    float submersion = 0;
    bool supported = false;
    glm::vec3 support_normal{0, 0, 1};
    glm::vec3 surface_normal{0, 0, 1};
    bool walkable_support = false; // a walkable or step contact (the jump is allowed on it)
    bool steep_only = false;
    uint8_t ground_layer = 0;
    uint32_t ground_material = 0;

    Impl(World& w, JPH::PhysicsSystem& s) : world(w), system(s) {}

    // The hull turns with the character's heading: its octagon's vertices sit at the heading plus multiples of
    // 45 degrees (measured: slopes3/stairs2 contacts, the game's hull vertex touching a stair edge lies where the
    // turned octagon has one and the axis-aligned one has none).
    JPH::RMat44 hull_transform(const glm::vec3& at) const {
        return JPH::RMat44::sRotationTranslation(JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), turn),
                                                 JPH::RVec3(at.x, at.y, at.z + centre));
    }

    // Bethesda's collector (bhkCharacterPointCollector::addCdPoint): a triangle contact on a back face is dropped,
    // a face contact (its normal within about 0.5 degrees of the face's) is tagged 1 and any other triangle
    // contact 2; other shapes are tagged 0. Every triangle contact, kept or dropped, is followed by the filter.
    void collect(std::vector<Contact>& out, const std::vector<Contact>* linked, const JPH::BodyID& body,
                 const JPH::SubShapeID& sub, glm::vec3 position, glm::vec3 normal, float distance,
                 float fraction = 0.0f) const {
        JPH::BodyLockRead lock(system.GetBodyLockInterfaceNoLock(), body);
        if (!lock.Succeeded()) {
            return;
        }
        const JPH::Body& b = lock.GetBody();
        Contact c;
        c.position = position;
        c.normal = normal;
        c.distance = distance;
        c.fraction = fraction;
        c.body = body;
        c.sub = sub;
        const uint64_t ud = b.GetShape()->GetSubShapeUserData(sub);
        c.layer = user_data_layer(ud);
        c.material = user_data_material(ud);
        // Havok's surface lies the shape's convex radius out, and its contact points lie on that surface.
        const float shell = shell_of(b, ud);
        c.distance -= shell;
        c.position += shell * normal;
        if (b.IsKinematic()) {
            c.keyframed = true;
            const glm::vec3 r = c.position - to_glm(JPH::Vec3(b.GetCenterOfMassPosition()));
            c.velocity = to_glm(b.GetLinearVelocity()) + glm::cross(to_glm(b.GetAngularVelocity()), r);
        }
        if (b.IsDynamic()) {
            c.dynamic = true;
            const float inv = b.GetMotionProperties()->GetInverseMass();
            c.mass = inv > 0 ? 1.0f / inv : 0.0f;
        }
        JPH::SubShapeID remainder;
        const JPH::Shape* leaf = b.GetShape()->GetLeafShape(sub, remainder);
        if (!is_triangle(leaf)) {
            out.push_back(c);
            return;
        }
        const glm::vec3 face = to_glm(b.GetWorldSpaceSurfaceNormal(sub, JPH::RVec3(position.x, position.y, position.z)));
        const float d = glm::dot(face, normal);
        if (d >= 0.0f) {
            c.tag = d > 0.999962f ? 1 : 2;
            out.push_back(c);
        }
        filter(out, linked);
    }

    // The collector's internal-edge filter: a contact that is not a face contact goes when it lies behind the
    // plane of a face contact at least two places before it (the game's loop skips the one right before; measured:
    // an edge contact collected right after a face contact survives), and when it lies behind a face contact of
    // the linked collector (the cast's: the start points). A removal moves the last contact into the gap.
    static void filter(std::vector<Contact>& out, const std::vector<Contact>* linked) {
        auto behind = [](const Contact& face, const Contact& c) {
            return face.tag == 1 && c.tag != 1 && glm::dot(face.position - c.position, face.normal) > 0.0f;
        };
        auto remove = [&](size_t i) {
            out[i] = out.back();
            out.pop_back();
        };
        for (size_t i = out.size(); i-- > 1;) {
            for (size_t j = i - 1; j-- > 0;) {
                if (behind(out[j], out[i])) {
                    remove(i);
                    break;
                }
            }
        }
        if (linked) {
            for (size_t l = linked->size(); l-- > 0;) {
                for (size_t i = out.size(); i-- > 0;) {
                    if (behind((*linked)[l], out[i])) {
                        remove(i);
                    }
                }
            }
        }
    }

    // A world shape's Havok shell: its convex radius as authored, whatever the reference's scale (measured: a
    // pine at scale 0.74 reports its trunk contacts 0.005 off the bare mesh, its unscaled shell).
    static float shell_of(const JPH::Body&, uint64_t user_data) { return user_data_radius(user_data); }

    float shape_radius(const JPH::BodyID& body, const JPH::SubShapeID& sub) const {
        JPH::BodyLockRead lock(system.GetBodyLockInterfaceNoLock(), body);
        return lock.Succeeded() ? shell_of(lock.GetBody(), lock.GetBody().GetShape()->GetSubShapeUserData(sub)) : 0.0f;
    }

    // A triangle's rank in Havok's collection order (osp RANK), 0 for shapes without one.
    uint32_t collection_rank(const JPH::BodyID& body, const JPH::SubShapeID& sub) const {
        JPH::BodyLockRead lock(system.GetBodyLockInterfaceNoLock(), body);
        if (!lock.Succeeded()) {
            return 0;
        }
        JPH::SubShapeID remainder;
        const JPH::Shape* leaf = lock.GetBody().GetShape()->GetLeafShape(sub, remainder);
        if (!leaf || leaf->GetSubType() != JPH::EShapeSubType::Mesh || !(leaf->GetUserData() & k_user_data_ranked)) {
            return 0;
        }
        return static_cast<const JPH::MeshShape*>(leaf)->GetTriangleUserData(remainder);
    }

    // The closest points between the hull at `at` and every shape within `reach` of it (Havok distances: both
    // shells off), in body and sub-shape order.
    struct Closest {
        JPH::BodyID body;
        JPH::SubShapeID sub;
        float distance = 0; // between the Havok surfaces
        float shell = 0;    // the shape's convex radius
        glm::vec3 normal{0, 0, 1};
        glm::vec3 point{0}; // on the bare shape
    };
    std::vector<Closest> closest_points(const glm::vec3& at, float reach) const {
        JPH::CollideShapeSettings s;
        s.mMaxSeparationDistance = reach + convex_radius + k_max_shape_radius;
        s.mActiveEdgeMode = JPH::EActiveEdgeMode::CollideWithAll;
        s.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
        JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> col;
        const JPH::RMat44 com = hull_transform(at).PreTranslated(hull->GetCenterOfMass());
        system.GetNarrowPhaseQueryNoLock().CollideShape(hull, JPH::Vec3::sReplicate(1.0f), com, s, JPH::RVec3::sZero(),
                                                       col);
        // Collected as Havok collects them: body by body, a mesh's triangles in its MOPP's order (the rank each
        // triangle carries), else in sub-shape order.
        struct Ordered {
            const JPH::CollideShapeResult* hit;
            uint32_t rank;
        };
        std::vector<Ordered> ordered;
        ordered.reserve(col.mHits.size());
        for (const auto& h : col.mHits) {
            ordered.push_back({&h, collection_rank(h.mBodyID2, h.mSubShapeID2)});
        }
        std::ranges::sort(ordered, [](const Ordered& a, const Ordered& b) {
            if (a.hit->mBodyID2 != b.hit->mBodyID2) {
                return a.hit->mBodyID2 < b.hit->mBodyID2;
            }
            if (a.rank != b.rank) {
                return a.rank < b.rank;
            }
            return a.hit->mSubShapeID2.GetValue() < b.hit->mSubShapeID2.GetValue();
        });
        std::vector<Closest> out;
        out.reserve(col.mHits.size());
        for (const Ordered& o : ordered) {
            const JPH::CollideShapeResult& h = *o.hit;
            Closest c;
            c.body = h.mBodyID2;
            c.sub = h.mSubShapeID2;
            c.shell = shape_radius(h.mBodyID2, h.mSubShapeID2);
            c.distance = -h.mPenetrationDepth - convex_radius - c.shell;
            c.normal = -to_glm(h.mPenetrationAxis.NormalizedOr(JPH::Vec3::sAxisZ()));
            c.point = to_glm(JPH::Vec3(h.mContactPointOn2));
            if (c.distance < reach) {
                out.push_back(c);
            }
        }
        return out;
    }

    // The same closest points for one sub-shape of a body, if it lies within `reach`.
    bool closest_point(const glm::vec3& at, float reach, Closest& c) const {
        JPH::BodyLockRead lock(system.GetBodyLockInterfaceNoLock(), c.body);
        if (!lock.Succeeded()) {
            return false;
        }
        JPH::CollideShapeSettings s;
        s.mMaxSeparationDistance = reach + convex_radius + c.shell;
        s.mActiveEdgeMode = JPH::EActiveEdgeMode::CollideWithAll;
        s.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
        JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> col;
        const JPH::RMat44 com = hull_transform(at).PreTranslated(hull->GetCenterOfMass());
        lock.GetBody().GetTransformedShape().CollideShape(hull, JPH::Vec3::sReplicate(1.0f), com, s, JPH::RVec3::sZero(),
                                                          col);
        for (const auto& h : col.mHits) {
            if (h.mSubShapeID2 == c.sub) {
                const float distance = -h.mPenetrationDepth - convex_radius - c.shell;
                if (distance >= reach) {
                    return false;
                }
                c.distance = distance;
                c.normal = -to_glm(h.mPenetrationAxis.NormalizedOr(JPH::Vec3::sAxisZ()));
                c.point = to_glm(JPH::Vec3(h.mContactPointOn2));
                return true;
            }
        }
        return false;
    }

    // The hull set at `at` and swept along `d` (the shape phantom's setPositionAndLinearCast, each touched shape
    // through Havok's GSK linear cast): a surface within startPointTolerance of the hull is a start point; a
    // surface is hit when the path closes on it by more than maxExtraPenetration and would reach it, at fraction 0
    // when it lies within the early-out distance already, else where conservative advancement reaches it. Hits
    // come back in fraction order with their distance from `at` along their normal, −(n·d) × fraction. The hits are
    // filtered against the start points of this cast, or against `linked` when it collects none.
    void linear_cast(const glm::vec3& at, const glm::vec3& d, std::vector<Contact>* starts, std::vector<Contact>& hits,
                     const std::vector<Contact>* linked) const {
        const float reach = k_keep_distance + k_keep_tolerance + glm::length(d); // tolerance + path length
        if (starts) {
            starts->clear();
            linked = starts;
        }
        hits.clear();
        for (Closest c : closest_points(at, reach)) {
            if (starts && c.distance < k_keep_distance + k_keep_tolerance) {
                collect(*starts, nullptr, c.body, c.sub, c.point, c.normal, c.distance + c.shell);
            }
            const float closing = glm::dot(d, c.normal);
            const float end = c.distance + closing;
            if (!(end <= 0.0f && closing + k_max_extra_penetration < 0.0f)) {
                continue;
            }
            float f = 0.0f;
            if (c.distance > k_cast_early_out) {
                f = c.distance / (c.distance - end);
                bool hit = false;
                for (int left = k_cast_iterations - 1;;) {
                    if (!closest_point(at + f * d, reach, c)) {
                        break;
                    }
                    const float approach = -glm::dot(c.normal, d);
                    if (approach <= 0.0f || approach * f + c.distance > approach) {
                        break; // turning away, or not reached within the path
                    }
                    if (c.distance > k_cast_early_out && --left >= 0) {
                        f += c.distance / approach;
                        continue;
                    }
                    hit = true; // close enough, or out of iterations: at the position reached
                    break;
                }
                if (!hit) {
                    continue;
                }
            }
            collect(hits, linked, c.body, c.sub, c.point, c.normal, -glm::dot(c.normal, d) * f + c.shell, f);
        }
        std::ranges::stable_sort(hits, {}, &Contact::fraction);
    }

    // findSurface: the manifold point matching a contact (surface distance below 0.1), or -1.
    int find_surface(const Contact& c) const {
        int best = -1;
        float best_d = 0.1f;
        for (size_t i = 0; i < manifold.size(); ++i) {
            const float d = surface_distance(manifold[i], c);
            if (d < best_d) {
                best_d = d;
                best = int(i);
            }
        }
        return best;
    }

    // updateManifold: manifold points follow their nearest start point (surface distance below 1.1) or go; the
    // closest new start points and the first new cast hit join; duplicates go. Every removal moves the last
    // element into the gap, as Havok's arrays do, so the manifold keeps the game's order, which the solver's
    // tie-breaks follow (measured: stairs, two step edges swap places when the floor contact before them goes).
    void update_manifold(std::vector<Contact> starts, const std::vector<Contact>& hits) {
        auto remove = [](std::vector<Contact>& v, size_t i) {
            v[i] = v.back();
            v.pop_back();
        };
        float closest = std::numeric_limits<float>::max();
        for (const auto& s : starts) {
            closest = std::min(closest, s.distance);
        }
        for (size_t m = manifold.size(); m-- > 0;) {
            int best = -1;
            float best_d = 1.1f;
            for (size_t s = 0; s < starts.size(); ++s) {
                const float d = surface_distance(starts[s], manifold[m]);
                if (d < best_d) {
                    best_d = d;
                    best = int(s);
                }
            }
            if (best >= 0) {
                manifold[m] = starts[size_t(best)];
                remove(starts, size_t(best));
            } else {
                remove(manifold, m);
            }
        }
        for (const Contact& s : starts) {
            if (s.distance == closest) {
                if (const int m = find_surface(s); m >= 0) {
                    manifold[size_t(m)] = s;
                } else {
                    manifold.push_back(s);
                }
            }
        }
        if (!hits.empty() && find_surface(hits.front()) < 0) {
            manifold.push_back(hits.front());
        }
        for (size_t i = manifold.size(); i-- > 1;) {
            for (size_t j = i; j-- > 0;) {
                if (surface_distance(manifold[i], manifold[j]) < 0.1f) {
                    remove(manifold, i);
                    break;
                }
            }
        }
    }

    // extractSurfaceConstraintInfo: the plane kept keepDistance off the surface, moving as the surface does; a contact
    // inside that is pushed out at its depth per second.
    simplex::Plane plane_of(const Contact& c) const {
        simplex::Plane p;
        p.n = c.normal;
        p.d = c.distance - k_keep_distance;
        p.priority = c.keyframed ? 1 : 2; // between a moving object and a wall, the wall holds
        p.velocity = c.velocity;
        if (p.d < -k_eps) {
            p.velocity += (-p.d * k_penetration_recovery) * p.n;
            p.d = 0;
        }
        return p;
    }

    // a contact too steep to walk on is a step when the hull could stand on top of
    // it within the step height.
    bool is_step(const Contact& c) const {
        // A surface of a stairs material is a step wherever it is touched (the test's first rule).
        if (std::ranges::binary_search(stairs_materials, c.material)) {
            return true;
        }
        // The reference point (the feet) and the step height above it.
        const float h_top = feet.z + step;
        glm::vec3 h(feet.x - c.position.x, feet.y - c.position.y, 0.0f);
        if (glm::dot(h, h) < 1e-12f) {
            return false;
        }
        h = glm::normalize(h);
        if (glm::dot(c.normal, h) < 0.0f) {
            return false; // faces away from the character
        }
        if (c.position.z - h_top > step / 3.0f) {
            return false;
        }
        JPH::BodyLockRead lock(system.GetBodyLockInterfaceNoLock(), c.body);
        if (!lock.Succeeded()) {
            return false;
        }
        const JPH::Body& body = lock.GetBody();
        JPH::SubShapeID remainder;
        const JPH::Shape* leaf = body.GetShape()->GetLeafShape(c.sub, remainder);
        if (is_triangle(leaf)) {
            // A thin capsule at the step height from the contact outward: if it cuts the touched triangle, the
            // obstacle rises above the step height there.
            JPH::Shape::SupportingFace face;
            body.GetTransformedShape().GetSupportingFace(c.sub, to_jolt(-c.normal), JPH::RVec3::sZero(), face);
            if (face.size() < 3) {
                return false;
            }
            const glm::vec3 p(c.position.x, c.position.y, h_top);
            const glm::vec3 q = p - probe * h;
            const float d2 = segment_triangle_distance2(p, q, to_glm(face[0]), to_glm(face[1]), to_glm(face[2]));
            const float reach = k_step_capsule + shell_of(body, body.GetShape()->GetSubShapeUserData(c.sub));
            return d2 >= reach * reach;
        }
        // Other shapes: a ray at the step height from the reference point toward the contact.
        const JPH::RRayCast ray(JPH::RVec3(feet.x, feet.y, h_top), to_jolt(-(radius + probe) * h));
        JPH::RayCastResult hit;
        return !body.GetTransformedShape().CastRay(ray, hit);
    }

    // The game's constraint callback (bhkCharProxyController::processConstraintsCallback): classifies the
    // manifold's contacts as support, rewrites step planes into ramps, holds still characters on walkable ground
    // and appends a vertical wall for each steep contact. Updates the support normal and flags.
    void constraints(std::vector<simplex::Plane>& planes, bool moving, bool fast) {
        const size_t count = std::min(manifold.size(), planes.size());
        int support = 0, steep = 0, walls = 0;
        glm::vec3 sum(0, 0, 1);
        walkable_support = false;
        bool material_set = false;
        for (size_t i = 0; i < count; ++i) {
            const Contact& c = manifold[i];
            simplex::Plane& p = planes[i];
            if (c.layer == k_layer_animstatic) {
                p.extra_down_static_friction = k_extra_down_animstatic;
            }
            bool counts = false;
            const bool clutter = c.dynamic && (c.layer == k_layer_clutter || c.layer == k_layer_weapon ||
                                               c.layer == k_layer_projectile);
            if (clutter || (c.dynamic && c.layer == k_layer_props)) {
                // Dynamic clutter and props hold the character up like flat ground; light clutter (below
                // fMoveLimitMass) doesn't block at all, props slide freely (bhkCharProxyController's callback).
                if (clutter && c.mass < k_move_limit_mass) {
                    p.n = glm::vec3(0.0f);
                    p.d = 1e30f;
                    p.velocity = glm::vec3(0.0f);
                } else if (!clutter) {
                    p.dynamic_friction = 0;
                    p.velocity = glm::vec3(0.0f);
                }
                sum = support == 0 ? glm::vec3(0, 0, 1) : sum + glm::vec3(0, 0, 1);
                ++support;
                continue;
            }
            if (c.normal.z >= cos_walkable) {
                counts = true;
                walkable_support = true;
                if (!moving) {
                    p.extra_down_static_friction = k_extra_down_still;
                }
            } else if (c.normal.z >= 0.0f) {
                if (is_step(c)) {
                    counts = true;
                    walkable_support = true;
                    if (!fast) {
                        glm::vec4 q(p.n.x, p.n.y, 1.0f, p.d);
                        q /= glm::length(glm::vec3(q));
                        p.n = glm::vec3(q);
                        p.d = q.w;
                        p.dynamic_friction = 0;
                    }
                } else {
                    if (walls < k_max_walls) {
                        const float hl = std::sqrt(p.n.x * p.n.x + p.n.y * p.n.y);
                        if (hl > k_eps) {
                            simplex::Plane wall = p;
                            wall.n = glm::vec3(p.n.x / hl, p.n.y / hl, 0.0f);
                            wall.d = p.d / hl;
                            wall.velocity = glm::vec3(0.0f);
                            planes.push_back(wall);
                            ++walls;
                        }
                    }
                    if (c.normal.z >= k_steep_support) {
                        counts = true;
                        ++steep;
                    }
                }
            }
            if (counts) {
                sum = support == 0 ? planes[i].n : sum + planes[i].n;
                ++support;
                if (!material_set) {
                    ground_layer = c.layer;
                    ground_material = c.material;
                    material_set = c.material != 0;
                }
            }
        }
        support_normal = support > 0 ? glm::normalize(sum) : glm::vec3(0, 0, 1);
        steep_only = support == steep;
        supported = support > 0;
    }

    std::vector<simplex::Plane> planes() const {
        std::vector<simplex::Plane> out;
        out.reserve(manifold.size() + k_max_walls);
        for (const Contact& c : manifold) {
            out.push_back(plane_of(c));
        }
        return out;
    }

    // The support check (checkSupport): classify the manifold the last integrate left, and ask the solver
    // whether the hull can sink at all.
    void check_support(bool moving) {
        ground_layer = 0;
        ground_material = 0;
        std::vector<simplex::Plane> p = planes();
        constraints(p, moving, false);
        const bool callback_support = supported;
        for (auto& q : p) {
            q.velocity = glm::vec3(0.0f);
        }
        simplex::Input in;
        in.velocity = glm::vec3(0, 0, -1);
        in.delta_time = in.min_delta_time = 1.0f / 60.0f;
        simplex::Output out;
        simplex::solve(in, p, out);
        surface_normal = glm::vec3(0.0f);
        for (size_t i = 0; i < p.size() && i < out.interactions.size(); ++i) {
            if (out.interactions[i].touched && p[i].n.z > 0.08f) {
                surface_normal += p[i].n;
            }
        }
        surface_normal = glm::length(surface_normal) > 1e-6f ? glm::normalize(surface_normal) : glm::vec3(0, 0, 1);
        const glm::vec3 d = out.velocity - in.velocity;
        const bool havok_supported =
            !(std::abs(d.x) <= 0.001f && std::abs(d.y) <= 0.001f && std::abs(d.z) <= 0.001f);
        supported = callback_support || (havok_supported && !steep_only);
    }

    // The game with Bethesda's handler: after each
    // solve, every manifold contact with a simulated body gets Havok's impulse, 90 % of the closing speed at the
    // contact plus a penetration push, which the handler scales by the body's layer and mass. Bodies at
    // fMoveLimitMass or heavier get none, but are still woken.
    void resolve_contacts(const glm::vec3& v_c, float dt, bool moving) {
        JPH::BodyInterface& bi = system.GetBodyInterfaceNoLock();
        for (const Contact& c : manifold) {
            if (!c.dynamic) {
                continue;
            }
            JPH::Vec3 impulse = JPH::Vec3::sZero();
            {
                JPH::BodyLockRead lock(system.GetBodyLockInterfaceNoLock(), c.body);
                if (!lock.Succeeded() || !lock.GetBody().IsDynamic()) {
                    continue;
                }
                const JPH::Body& b = lock.GetBody();
                const float inv_mass = b.GetMotionProperties()->GetInverseMass();
                const glm::vec3 r = c.position - to_glm(JPH::Vec3(b.GetCenterOfMassPosition()));
                const glm::vec3 vp = to_glm(b.GetLinearVelocity()) + glm::cross(to_glm(b.GetAngularVelocity()), r);
                const float u = glm::dot(vp - v_c, c.normal);
                float j0 = -0.9f * u;
                if (c.distance < 0) {
                    j0 += 0.4f * c.distance / dt;
                }
                const float m = inv_mass > 0 ? 1.0f / inv_mass : 0.0f;
                if (j0 < 0 && m > 0 && m < k_move_limit_mass) {
                    const glm::vec3 rn = glm::cross(r, c.normal);
                    const float k_eff = inv_mass + glm::dot(rn, to_glm(b.GetInverseInertia().Multiply3x3(to_jolt(rn))));
                    glm::vec3 j = (j0 / k_eff) * c.normal;
                    const float size = glm::length(j);
                    float k = 2.0f;
                    if (c.layer == k_layer_projectile) {
                        k = 0.0f;
                    } else if (c.layer == k_layer_props) {
                        k = m <= 0.5f * k_move_limit_mass ? 3.0f : 0.1f;
                    } else if (c.layer == k_layer_trap) {
                        k = 0.1f;
                    }
                    if (size / m > 1000.0f) {
                        k *= 1000.0f / (size / m);
                    }
                    // A moving character's head-high contacts push along its velocity instead
                    // (bhkBipedOrientationController).
                    if (moving && c.position.z - feet.z > 0.9f * height && glm::dot(v_c, v_c) > 0) {
                        j = glm::normalize(v_c) * size;
                    }
                    impulse = to_jolt(k * j);
                }
            }
            if (impulse.IsNearZero(0.0f)) {
                bi.ActivateBody(c.body);
            } else {
                bi.AddImpulse(c.body, impulse, JPH::RVec3(to_jolt(c.position)));
            }
        }
    }

    // integrate: up to ten rounds of contacts, solve and sweep, each from the same proxy velocity.
    void integrate(float dt, bool moving) {
        const glm::vec3 v_c = velocity;
        const bool fast = glm::dot(velocity, velocity) > k_fast;
        float remaining = dt;
        simplex::Output out;
        bool solved = false;
        std::vector<Contact> starts, hits, moved;
        for (int it = 0; it < k_max_cast_iterations && remaining > k_eps; ++it) {
            linear_cast(feet, old_displacement, &starts, hits, nullptr);
            update_manifold(starts, hits);
            std::vector<simplex::Plane> p = planes();
            constraints(p, moving, fast);
            simplex::Input in;
            in.velocity = velocity;
            in.delta_time = remaining;
            const float speed = glm::length(velocity);
            in.min_delta_time = speed > 0 ? 0.5f * k_keep_distance / speed : 0.0f;
            simplex::solve(in, p, out);
            solved = true;
            resolve_contacts(v_c, dt, moving);
            const glm::vec3 d = out.position;
            const glm::vec3 diff = glm::abs(d - old_displacement);
            if (diff.x <= 0.001f && diff.y <= 0.001f && diff.z <= 0.001f) {
                feet += d;
                remaining -= out.delta_time;
            } else {
                linear_cast(feet, d, nullptr, moved, &starts);
                const Contact* first = nullptr;
                for (const Contact& h : moved) {
                    if (find_surface(h) < 0) {
                        first = &h;
                        break;
                    }
                }
                if (!first) {
                    feet += d;
                    remaining -= out.delta_time;
                } else {
                    manifold.push_back(*first);
                    const float approach = -glm::dot(first->normal, d);
                    const float f = std::clamp(first->fraction - k_keep_distance / std::max(approach, k_eps), 0.0f, 1.0f);
                    feet += f * d;
                    remaining -= f * out.delta_time;
                }
            }
            old_displacement = d;
        }
        if (solved) {
            velocity = out.velocity;
        }
    }
};

JPH::Ref<JPH::Shape> character_hull(const glm::vec3& half_extents, float step_up) {
    // The game's hull: a point at the bottom, an octagon of the mean
    // horizontal half extent a step height plus 0.1 up (the cone that rides up step edges), the same octagon half
    // a radius below the top, and a point at the top; its centre 2 x keepDistance above the half height, so the
    // bottom point is 0.1 above the feet.
    const float hz = std::max(half_extents.z, 0.1f);
    const float r = std::max(0.5f * (half_extents.x + half_extents.y), 0.05f);
    JPH::Array<JPH::Vec3> points;
    points.push_back(JPH::Vec3(0, 0, -hz));
    // The octagon's directions as the game's tables hold them (g_CharShapeOctagonCos/Sin: 0.7071, not sqrt(2)/2).
    constexpr float k_oct_cos[8] = {1.0f, 0.7071f, 0.0f, -0.7071f, -1.0f, -0.7071f, 0.0f, 0.7071f};
    constexpr float k_oct_sin[8] = {0.0f, 0.7071f, 1.0f, 0.7071f, 0.0f, -0.7071f, -1.0f, -0.7071f};
    for (int i = 0; i < 8; ++i) {
        points.push_back(JPH::Vec3(r * k_oct_cos[i], r * k_oct_sin[i], -hz + step_up + 0.1f));
        points.push_back(JPH::Vec3(r * k_oct_cos[i], r * k_oct_sin[i], hz - 0.5f * r));
    }
    points.push_back(JPH::Vec3(0, 0, hz));
    JPH::ConvexHullShapeSettings hull(points, 0.0f);
    return hull.Create().Get();
}

float character_hull_centre(const glm::vec3& half_extents) {
    return std::max(half_extents.z, 0.1f) + 2.0f * k_keep_distance;
}

Character::Character(World& world, const CharacterSettings& s, const glm::vec3& feet)
    : m_settings(s), m_impl(std::make_unique<Impl>(world, access(world).system)) {
    auto& m = *m_impl;
    const float hz = std::max(s.half_extents.z, 0.1f);
    const float r = std::max(0.5f * (s.half_extents.x + s.half_extents.y), 0.05f);
    m.hull = character_hull(s.half_extents, s.step_up);
    m.centre = character_hull_centre(s.half_extents);
    m.convex_radius = s.convex_radius;
    m.radius = r;
    m.height = 2.0f * hz;
    m.step = s.step_up;
    const float cos47 = std::cos(47.0f * std::numbers::pi_v<float> / 180.0f);
    m.probe = s.step_up * cos47 / std::sin(cos47); // as the game computes it
    m.stairs_materials = s.stairs_materials;
    m.cos_walkable = std::cos(s.max_slope_deg * std::numbers::pi_v<float> / 180.0f);
    m.feet = feet;
}

Character::~Character() = default;

void Character::set_velocity(const glm::vec3& velocity) { m_impl->velocity = velocity; }

void Character::set_position(const glm::vec3& feet) {
    // As the game's controller position setter (reached by console setpos): only the contacts and
    // the last step's displacement go; state, velocity and the fall start carry on.
    auto& m = *m_impl;
    m.feet = feet;
    m.old_displacement = glm::vec3(0.0f);
    m.manifold.clear();
}

void Character::place(const glm::vec3& feet) {
    // Actor::SetPosition with its update flag: the controller's SetPosition, SetLinearVelocity(0), the
    // fall start 0 (no fall start: the next landing measures no fall) and the landed flag cleared
    set_position(feet);
    m_impl->velocity = glm::vec3(0.0f);
    m_impl->fall_start = 0;
    m_impl->landed = false;
}

void Character::step(const ControllerInput& in, float dt) {
    auto& m = *m_impl;
    const glm::vec3 up(0, 0, 1);
    m.landed = false;
    if (glm::dot(in.facing, in.facing) > 0) {
        m.turn = std::atan2(in.facing.y, in.facing.x) - 0.5f * std::numbers::pi_v<float>;
    }
    // Idle: no axis of this step's requested displacement beyond 0.05 units and no external velocity beyond
    // 0.001 (flag 0x8 "moving" is its opposite). An idle character's
    // vertical velocity read back from the proxy is dropped at the start of the step unless it is in the air,
    // so standing on a slope too steep to hold it slides one step's gravity at a time.
    const glm::vec3 displacement = in.desired * (dt / k_unit);
    const bool idle = glm::all(glm::lessThanEqual(glm::abs(displacement), glm::vec3(0.05f))) &&
                      glm::all(glm::lessThanEqual(glm::abs(in.external), glm::vec3(0.001f)));
    const bool moving = !idle;
    glm::vec3 v = m.velocity;
    if (idle && m.state != ControllerState::in_air) {
        v.z = 0;
    }

    // The support check, on the manifold the last integrate left.
    m.check_support(moving);
    // The take-off window: no support while still moving up off the ground.
    if (m.take_off) {
        if (v.z <= 0.0f || (m.walkable_support && glm::dot(glm::normalize(v), m.support_normal) <= 0.05f)) {
            m.take_off = false;
        } else {
            m.supported = false;
        }
    }
    const bool supported = m.supported;
    const glm::vec3 normal = m.support_normal;
    const float z = m.feet.z;
    const glm::vec2 desired_h(in.desired);

    // Water (the actor's water update): swimming starts once the water reaches 0.7 of the body height and ends
    // when the character stands on something below that, or falls clear of the water.
    m.submersion = 0;
    const float water_z = std::isfinite(in.water_feet_z) ? in.water_feet_z : z;
    if (std::isfinite(in.water_height) && in.body_height > 0 && in.water_height > water_z) {
        m.submersion = std::min((in.water_height - water_z) / in.body_height, 1.0f);
    }
    bool entering_water = false;
    bool leaving_to_air = false;
    if (m.submersion >= 0.7f) {
        if (!m.swimming || m.state == ControllerState::on_ground || m.state == ControllerState::in_air) {
            if (m.state != ControllerState::swimming) {
                m.fall_start = 0;
                entering_water = true; // the step that switches keeps its velocity (measured: a plunge goes one
                                       // more full step below the swimming height)
            }
            m.state = ControllerState::swimming;
            m.jump_pending = false;
        }
        m.swimming = true;
    } else if (m.swimming || m.state == ControllerState::swimming) {
        if (!supported && m.state != ControllerState::in_air) {
            if (m.submersion < 0.01f) {
                // The water update requests InAir; the swim state still runs this step and commits the request at
                // the end of its update (bhkCharacterStateSwimming::Update). The fall start stays as
                // swimming left it, 0, until the first InAir step raises it (measured: water6 fall_into_water, one
                // step at surface gravity, then one at normal gravity, then the falling gravity).
                leaving_to_air = true;
            }
        } else {
            m.swimming = false;
            if (m.state != ControllerState::in_air) {
                m.state = ControllerState::on_ground;
            }
        }
    }

    const ControllerState before = m.state;
    if (m.state == ControllerState::in_air && supported) {
        m.state = ControllerState::on_ground; // landing
        m.landed = true;
        // none when the fall start is exactly 0 (unset).
        m.fall_distance = m.fall_start == 0.0f ? 0.0f : std::max(m.fall_start - z, 0.0f);
        m.air_time = 0;
    }

    bool run_air = false;
    float gravity_mult = 1.0f;
    if (entering_water) {
        // velocity as it was
    } else if (m.jump_pending) {
        // Take-off: the horizontal velocity carries on, the jump speed goes up, and the first air step follows.
        m.jump_pending = false;
        v.z = in.jump_speed;
        m.fall_start = z;
        m.state = ControllerState::in_air;
        m.take_off = true;
        run_air = true;
    } else if (m.state == ControllerState::on_ground) {
        const bool settled = m.previous_state == ControllerState::on_ground;
        if (in.jump && m.walkable_support && settled) {
            m.state = ControllerState::jumping; // takes off on the next step
            m.jump_pending = true;
        } else if (!supported) {
            // Lost the ground: within the cast length below, stay on it and fall faster onto it; else fall.
            const auto hit = m.world.raycast(m.feet + up * 0.01f, -up, in.unsupport_cast + 0.01f);
            if (hit) {
                gravity_mult = in.unsupported_gravity_mult;
            } else {
                m.state = ControllerState::in_air;
                m.fall_start = z;
                v.z = 0;
                run_air = true;
            }
        }
        if (!run_air) {
            // The movement utility in the support surface's frame: forward is the facing direction projected
            // onto the surface in its vertical plane, side the surface direction across it; the requested
            // velocity replaces the frame's in-surface components and its normal component. Then the vertical
            // velocity from before comes back, gravity is added, and upward velocity is moved off along the
            // support normal.
            const float speed = glm::length(desired_h);
            glm::vec3 w(0.0f);
            if (speed > 0) {
                const glm::vec3 facing = glm::vec3(in.facing, 0.0f);
                const glm::vec3 right = glm::cross(facing, up);
                const glm::vec3 t = glm::normalize(glm::cross(normal, right));
                const glm::vec3 b = glm::normalize(glm::cross(t, normal));
                const float f = glm::dot(glm::vec3(desired_h, 0.0f), facing);
                const float s = glm::dot(glm::vec3(desired_h, 0.0f), right);
                w = f * t + s * b;
            }
            w.z = v.z;
            v = w - up * (in.gravity * gravity_mult * dt) + in.external;
            if (v.z > 0.001f && std::abs(normal.z) > k_eps) {
                v -= normal * (v.z / normal.z);
            }
        }
    } else if (m.state == ControllerState::in_air) {
        run_air = true;
    } else if (m.state == ControllerState::swimming) {
        // The requested velocity in all three axes (the caller turns it by the view pitch). Under water there is
        // no gravity and no buoyancy; at the surface the swimmer can't rise and gravity holds it there, and a
        // jump can leave the water.
        // The movement utility runs in the support surface's frame once the swimmer has support (flag 0x100: the
        // surface normal), else in the world's: the requested forward and side speeds along the surface,
        // the requested vertical speed along its normal (bhkCharacterStateSwimming::Update; measured:
        // water6 swim_to_shore, the swimmer slows from 41 to 35 u/s once the shore's slope holds it).
        v = in.desired;
        const glm::vec3 facing(in.facing, 0.0f);
        if (supported && glm::dot(facing, facing) > 0) {
            const glm::vec3 right = glm::cross(facing, up);
            const glm::vec3 t = glm::normalize(glm::cross(normal, right));
            const glm::vec3 b = glm::normalize(glm::cross(t, normal));
            v = glm::dot(glm::vec3(desired_h, 0.0f), facing) * t + glm::dot(glm::vec3(desired_h, 0.0f), right) * b +
                in.desired.z * normal;
        }
        v += in.external;
        if (m.submersion < 0.7f) {
            v.z = std::min(v.z, 0.0f) - in.gravity * dt;
            if (in.jump) {
                m.state = ControllerState::jumping;
                m.jump_pending = true;
            }
        }
    }
    if (leaving_to_air && m.state == ControllerState::swimming) {
        m.swimming = false;
        m.state = ControllerState::in_air;
    }
    if (run_air) {
        if (z > m.fall_start) {
            m.fall_start = z; // still climbing: normal gravity
            gravity_mult = 1.0f;
        } else {
            gravity_mult = in.falling_gravity_mult;
        }
        v.z -= in.gravity * gravity_mult * dt;
        const glm::vec2 vh(v);
        const glm::vec2 nh = vh + in.air_control * (desired_h - vh);
        v.x = nh.x;
        v.y = nh.y;
        m.air_time += dt;
    }
    m.previous_state = before;

    m.velocity = m.requested_velocity = v;
    m.integrate(dt, moving);
}

CharacterState Character::state() const {
    const auto& m = *m_impl;
    CharacterState s;
    s.position = m.feet;
    s.velocity = m.velocity;
    s.requested_velocity = m.requested_velocity;
    s.state = m.state;
    s.on_ground = m.state == ControllerState::on_ground && m.supported;
    s.on_steep = m.supported && m.steep_only;
    s.supported = m.supported;
    s.fall_start = m.fall_start;
    s.air_time = m.air_time;
    s.landed = m.landed;
    s.fall_distance = m.fall_distance;
    s.swimming = m.swimming;
    s.submersion = m.submersion;
    s.ground_normal = m.support_normal;
    s.surface_normal = m.surface_normal;
    for (const auto& c : m.manifold) {
        s.manifold.push_back({c.position, c.normal, c.distance, c.tag, c.layer});
    }
    s.ground_layer = m.ground_layer;
    s.ground_material = m.ground_material;
    return s;
}

} // namespace spud::physics
