#include "physics/jolt.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "formats/osp.hpp"

#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <numbers>
#include <thread>

namespace spud::physics {

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return broad_layer::count; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == object_layer::fixed   ? broad_layer::fixed
               : layer == object_layer::actor ? broad_layer::actor
                                              : broad_layer::moving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == broad_layer::fixed ? "fixed" : layer == broad_layer::actor ? "actor" : "moving";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::BroadPhaseLayer b) const override {
        return a != object_layer::actor && b != broad_layer::actor && (a == object_layer::moving || b == broad_layer::moving);
    }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a != object_layer::actor && b != object_layer::actor && (a == object_layer::moving || b == object_layer::moving);
    }
};

// Every query but the character proxy's: actors answer those their own way (their own hits, what they see).
class NoActors final : public JPH::BroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::BroadPhaseLayer layer) const override { return layer != broad_layer::actor; }
};
const NoActors k_no_actors;

constexpr uint32_t k_charcontroller = 30;
constexpr uint64_t k_trap_layers = (uint64_t(1) << 14) | (uint64_t(1) << 16); // L_TRAP, L_CLOUDTRAP
constexpr uint32_t k_ragdoll_bodies = 4096;
constexpr float k_object_rest_reach = 5.0f; // metres
constexpr uint8_t k_dead_biped = 32; // L_DEADBIP: the character controller's COLL row leaves it out
constexpr float k_terrain_drop = 0.0125f;  // metres (Havok units)
constexpr float k_terrain_radius = 0.005f;
constexpr uint8_t k_leaf_layer = 2;  // L_ANIMSTATIC, as a door's own collision
constexpr float k_leaf_jump_m = 0.5f; // further than any door's leaf moves in a step
constexpr float k_actor_jump_m = 0.5f;           // further than any actor walks in a step
constexpr float k_actor_step_m = 31.0f * 0.0142875f; // the hull's step height: max(31 units, 0.75 r)
constexpr float k_actor_shell_m = 0.05f;         // g_HavokConvexRadius
constexpr uint8_t k_land_layer = 17; // L_GROUND: the land's heightfield (measured: the game's contacts with it)
constexpr float k_box_shell = 0.05f; // g_HavokConvexRadius

} // namespace

uint64_t character_layers(const uint64_t* rows, const uint32_t* flags, size_t count) {
    uint64_t mask = k_charcontroller < count ? rows[k_charcontroller] : 0;
    for (size_t i = 0; i < count && i < 64; ++i) {
        if (flags[i] & 0x3) { // trigger volume or sensor: overlaps only, never a solid contact
            mask &= ~(uint64_t(1) << i);
        }
    }
    return mask;
}

uint64_t default_character_layers() {
    // the game's built-in CHARCONTROLLER row.
    uint64_t m = 0;
    for (int l : {1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 16, 17, 20, 21, 22, 24, 26, 27, 28, 30, 31, 33, 34, 35,
                  38, 39, 40, 43, 44, 45, 46}) {
        m |= uint64_t(1) << l;
    }
    return m;
}

namespace {

JPH::Quat to_quat(const float q[4]) {
    JPH::Quat r(q[0], q[1], q[2], q[3]);
    const float len = r.Length();
    return len > 1e-6f && std::isfinite(len) ? r / len : JPH::Quat::sIdentity();
}

// Orientation taking Jolt's Y axis (capsules, cylinders) onto `dir`.
JPH::Quat y_to(const JPH::Vec3& dir) { return JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), dir); }

struct Built {
    JPH::RefConst<JPH::Shape> shape; // null: nothing solid
    uint64_t bytes = 0;
    uint32_t dropped = 0; // shapes Jolt could not build
    bool dynamic = false; // one simulated rigid body (clutter): it falls, sleeps and can be pushed
    float mass = 0;       // its mass, as authored (Havok kg)
    float friction = 0.5f, restitution = 0.4f; // its bhkRigidBody's
};

Built build_shape(const osp::Collision& c, uint64_t character_mask) {
    Built out;
    JPH::StaticCompoundShapeSettings compound;
    JPH::Ref<JPH::Shape> single;
    JPH::Vec3 single_pos = JPH::Vec3::sZero();
    JPH::Quat single_rot = JPH::Quat::sIdentity();
    int count = 0;
    auto vec = [](const float* p) { return JPH::Vec3(p[0], p[1], p[2]); };
    for (const auto& s : c.shapes) {
        if (s.layer >= 64 || !(character_mask & (uint64_t(1) << s.layer))) {
            continue;
        }
        JPH::ShapeSettings::ShapeResult result;
        JPH::Vec3 pos = JPH::Vec3::sZero();
        JPH::Quat rot = JPH::Quat::sIdentity();
        switch (s.type) {
        case osp::ShapeType::box: {
            const JPH::Vec3 half = JPH::Vec3::sMax(vec(s.extents), JPH::Vec3::sReplicate(0.001f));
            result = JPH::BoxShapeSettings(half, 0.0f).Create();
            pos = vec(s.centre);
            rot = to_quat(s.rotation);
            break;
        }
        case osp::ShapeType::sphere:
            result = JPH::SphereShapeSettings(std::max(s.radius, 0.001f)).Create();
            pos = vec(s.centre);
            break;
        case osp::ShapeType::capsule:
        case osp::ShapeType::cylinder: {
            const JPH::Vec3 a = vec(s.centre), b = vec(s.extents);
            const JPH::Vec3 d = b - a;
            const float len = d.Length();
            const float r = std::max(s.radius, 0.001f);
            pos = 0.5f * (a + b);
            if (len < 1e-4f) {
                result = s.type == osp::ShapeType::capsule
                             ? JPH::SphereShapeSettings(r).Create()
                             : JPH::CylinderShapeSettings(0.001f, r, 0.0f).Create();
            } else {
                rot = y_to(d / len);
                result = s.type == osp::ShapeType::capsule
                             ? JPH::CapsuleShapeSettings(0.5f * len, r).Create()
                             : JPH::CylinderShapeSettings(0.5f * len, r, 0.0f).Create();
            }
            break;
        }
        case osp::ShapeType::convex: {
            JPH::Array<JPH::Vec3> points;
            points.reserve(s.vertex_count);
            for (uint32_t v = 0; v < s.vertex_count; ++v) {
                points.push_back(vec(&c.vertices[(size_t(s.first_vertex) + v) * 3]));
            }
            JPH::ConvexHullShapeSettings hull(points, 0.0f);
            result = hull.Create();
            break;
        }
        case osp::ShapeType::mesh: {
            JPH::VertexList verts;
            verts.reserve(s.vertex_count);
            for (uint32_t v = 0; v < s.vertex_count; ++v) {
                const float* p = &c.vertices[(size_t(s.first_vertex) + v) * 3];
                verts.push_back(JPH::Float3(p[0], p[1], p[2]));
            }
            JPH::IndexedTriangleList tris;
            tris.reserve(s.index_count / 3);
            // Each triangle carries its rank in Havok's collection order (osp RANK) for the character
            // controller's contact order.
            const bool ranked = !c.ranks.empty();
            for (uint32_t i = 0; i < s.index_count; i += 3) {
                const uint32_t* t = &c.indices[size_t(s.first_index) + i];
                const uint32_t rank = ranked ? c.ranks[(size_t(s.first_index) + i) / 3] : 0;
                tris.push_back(JPH::IndexedTriangle(t[0], t[1], t[2], 0, rank));
            }
            JPH::MeshShapeSettings mesh(std::move(verts), std::move(tris));
            mesh.mPerTriangleUserData = ranked;
            result = mesh.Create();
            break;
        }
        }
        if (result.HasError()) {
            ++out.dropped;
            continue;
        }
        JPH::Ref<JPH::Shape> leaf = result.Get();
        // Spheres and capsules have no shell beyond their radius; boxes, cylinders, hulls and triangles do.
        const bool shell = s.type != osp::ShapeType::sphere && s.type != osp::ShapeType::capsule;
        leaf->SetUserData(shape_user_data(s.layer, s.material, shell ? s.convex_radius : 0.0f) |
                          (s.type == osp::ShapeType::mesh && !c.ranks.empty() ? k_user_data_ranked : 0));
        out.bytes += leaf->GetStats().mSizeBytes;
        compound.AddShape(pos, rot, leaf);
        if (count++ == 0) {
            single = leaf;
            single_pos = pos;
            single_rot = rot;
        }
    }
    if (count == 0) {
        return out;
    }
    // A model with a single rigid body that the game simulates (motion dynamic) gets a dynamic body.
    if (c.bodies.size() == 1 && c.bodies[0].motion == osp::Motion::dynamic && c.bodies[0].mass > 0) {
        out.dynamic = true;
        out.mass = c.bodies[0].mass;
        out.friction = c.bodies[0].friction;
        out.restitution = c.bodies[0].restitution;
    }
    if (count == 1 && single_pos.IsNearZero() && single_rot.IsClose(JPH::Quat::sIdentity())) {
        out.shape = single;
        return out;
    }
    auto result = compound.Create();
    if (result.HasError()) {
        ++out.dropped;
        return out;
    }
    out.shape = result.Get();
    return out;
}

} // namespace

void init_jolt() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!JPH::VerifyJoltVersionID()) {
            throw Error("Jolt was built with different feature defines than spud (see src/physics/CMakeLists.txt)");
        }
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

struct World::Impl {
    enum class CellState : uint8_t { none, requested, loaded };

    // Enable state against the records, read by the cell workers.
    mutable std::mutex ref_mutex;
    std::unordered_map<uint32_t, bool> ref_disabled;
    std::vector<uint32_t> rebuild;

    bool disabled(uint32_t form_id, uint32_t flags) const {
        std::lock_guard lock(ref_mutex);
        const auto it = ref_disabled.find(form_id);
        return it != ref_disabled.end() ? it->second : (flags & osw::ref_initially_disabled) != 0;
    }

    struct CellSlot {
        CellState state = CellState::none;
        bool cancelled = false;          // left the radius while loading
        std::vector<JPH::BodyID> bodies; // added to the system
        std::vector<uint32_t> models;    // model refs this cell holds
    };

    struct Model {
        enum class State : uint8_t { none, loading, ready };
        State state = State::none;
        uint32_t refs = 0;
        JPH::RefConst<JPH::Shape> shape;
        uint64_t bytes = 0;
        bool missing = false;
        bool dynamic = false;
        float mass = 0;
        float friction = 0.5f, restitution = 0.4f;
    };

    // Loader -> sim thread hand-off.
    struct Prepared {
        uint32_t cell;
        std::vector<JPH::BodyID> bodies;
        std::vector<JPH::BodyID> dynamic_bodies; // simulated from the moment the cell loads, until they settle
        std::vector<uint32_t> models;
        JPH::BodyInterface::AddState add_state = nullptr;
    };

    std::filesystem::path cache_root;
    std::shared_ptr<const osw::World> world;
    WorldSettings settings;

    BroadPhaseLayers broad_phase_layers;
    ObjectVsBroadPhase object_vs_broad_phase;
    ObjectPairs object_pairs;
    mutable std::shared_mutex query_mutex; // raycast_shared against bodies coming and going
    struct Ragdoll {
        std::vector<JPH::BodyID> bodies;
        std::vector<JPH::Ref<JPH::Constraint>> constraints;
        std::vector<float> force_scale;
    };
    // Other threads ask; update() does, and leaves the poses here.
    mutable std::mutex ragdoll_mutex;
    uint32_t next_ragdoll = 1;
    std::vector<std::pair<uint32_t, RagdollSpec>> ragdolls_to_add;
    std::vector<uint32_t> ragdolls_to_remove;
    struct RagdollPush {
        uint32_t id;
        glm::vec3 centre;
        float strength;
    };
    std::vector<RagdollPush> ragdolls_to_push;
    std::vector<std::pair<uint32_t, glm::vec3>> ref_impulses; // reference index, impulse
    std::vector<std::pair<uint32_t, glm::vec3>> ref_steers;   // reference index, velocity
    std::unordered_map<uint32_t, Ragdoll> ragdolls;
    std::unordered_map<uint32_t, std::vector<RagdollBodyState>> ragdoll_states;
    // Ragdoll bodies take ids past the world's, which go by cell and reference whenever those stream in.
    std::vector<uint32_t> free_body_ids;
    // Where moved clutter is, by reference index: it stays there when its cell comes back.
    std::unordered_map<uint32_t, RagdollBodyState> moved_refs;
    std::vector<MovedRef> moved_since_read;
    struct Object {
        uint32_t form_id = 0;
        std::future<Built> shape;
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        bool fixed = false;
    };
    std::vector<Object> objects_to_add;
    std::vector<uint32_t> objects_to_remove;
    std::unordered_map<uint32_t, JPH::BodyID> objects;
    struct AnimatedBody {
        uint32_t node = 0;
        JPH::RefConst<JPH::Shape> shape;
        JPH::BodyID id;
    };
    struct Animated {
        std::vector<AnimatedBody> bodies;
        bool added = false;
        bool moving = false; // moved last step
    };
    using AnimatedShapes = std::vector<AnimatedBody>;
    struct AnimatedLoad {
        uint32_t form_id = 0;
        std::future<AnimatedShapes> shapes;
    };
    std::vector<AnimatedLoad> animated_to_add;
    std::vector<uint32_t> animated_to_remove;
    std::unordered_map<uint32_t, Animated> animated;
    std::unordered_map<uint32_t, uint32_t> animated_by_body; // body index to reference
    std::unordered_map<uint32_t, std::vector<NodePose>> animated_moves;
    std::unordered_map<uint32_t, std::vector<NodePose>> animated_poses; // the last heard: a body may load after them

    // The game for two bodies of one actor on a biped layer: by body part, from the table
    // The game fills at filter, or with both marked 0x8000, unless they're neighbours.
    static bool parts_collide(uint32_t a, uint32_t b) {
        static constexpr uint32_t k_parts[32] = {
    0x00000000u, 0x004230C0u, 0x014030C0u, 0x014030C0u,
    0x014230C0u, 0x00403000u, 0x0042791Eu, 0x0041FF1Eu,
    0x0145F0C0u, 0x0145E080u, 0x0144E080u, 0x004000C0u,
    0x004241FEu, 0x0045C7FEu, 0x014437C0u, 0x01442780u,
    0x01442380u, 0x00401052u, 0x0041E700u, 0x00000000u,
    0x00400000u, 0x00400000u, 0x0037FFFEu, 0x00000000u,
    0x0001C71Cu, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
        };
        const uint32_t part_a = a >> 8 & 0x1F, part_b = b >> 8 & 0x1F;
        if (a & b & 0x8000) {
            return (part_a > part_b ? part_a - part_b : part_b - part_a) != 1;
        }
        return (k_parts[part_a] >> part_b & 1) != 0;
    }

    // The nearest two points of the capsules' segments, by stepping along them: bodies are few, and this runs once a death.
    static bool capsules_touch(const RagdollBodySpec& a, const RagdollBodySpec& b) {
        const glm::vec3 a0 = a.position + a.rotation * a.a, a1 = a.position + a.rotation * a.b;
        const glm::vec3 b0 = b.position + b.rotation * b.a, b1 = b.position + b.rotation * b.b;
        constexpr int k_steps = 8;
        constexpr float k_margin = 0.02f; // metres, Jolt's convex radius and a little
        const float reach = a.radius + b.radius + k_margin;
        for (int i = 0; i <= k_steps; ++i) {
            const glm::vec3 p = glm::mix(a0, a1, float(i) / k_steps);
            const glm::vec3 d = b1 - b0;
            const float length = glm::dot(d, d);
            const float t = length > 0.0f ? glm::clamp(glm::dot(p - b0, d) / length, 0.0f, 1.0f) : 0.0f;
            if (glm::distance(p, b0 + d * t) < reach) {
                return true;
            }
        }
        return false;
    }

    void add_ragdoll_now(uint32_t id, const RagdollSpec& spec) {
        auto& bi = system.GetBodyInterface();
        Ragdoll r;
        JPH::Ref<JPH::GroupFilterTable> filter = new JPH::GroupFilterTable(uint32_t(spec.bodies.size()));
        for (uint32_t i = 0; i < uint32_t(spec.bodies.size()); ++i) {
            for (uint32_t j = 0; j < i; ++j) {
                // Limbs that start inside each other stay out of it: the game has bhkRagdollPenetrationUtil for those, and
                // here the solver would throw the body across the room.
                if (!parts_collide(spec.bodies[i].collision_filter, spec.bodies[j].collision_filter) ||
                    capsules_touch(spec.bodies[i], spec.bodies[j])) {
                    filter->DisableCollision(i, j);
                }
            }
        }
        for (size_t i = 0; i < spec.bodies.size(); ++i) {
            const RagdollBodySpec& b = spec.bodies[i];
            // A capsule along its own axis: Jolt's stands on y, so a rotated one in place of Havok's two end points.
            JPH::BodyCreationSettings c(capsule(b.a, b.b, b.radius, k_dead_biped), JPH::RVec3(b.position.x, b.position.y, b.position.z),
                                        JPH::Quat(b.rotation.x, b.rotation.y, b.rotation.z, b.rotation.w),
                                        JPH::EMotionType::Dynamic, object_layer::moving);
            c.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
            c.mMassPropertiesOverride.mMass = b.mass;
            c.mMassPropertiesOverride.mInertia = JPH::Mat44::sScale(to_jolt(b.inertia));
            c.mFriction = b.friction;
            c.mRestitution = b.restitution;
            c.mLinearDamping = b.linear_damping;
            c.mAngularDamping = b.angular_damping;
            c.mGravityFactor = b.gravity_factor;
            c.mMaxLinearVelocity = b.max_linear_velocity;
            c.mMaxAngularVelocity = b.max_angular_velocity;
            c.mLinearVelocity = to_jolt(b.linear_velocity);
            c.mAngularVelocity = to_jolt(b.angular_velocity);
            c.mMotionQuality = JPH::EMotionQuality::LinearCast;
            c.mCollisionGroup = JPH::CollisionGroup(filter, id, JPH::CollisionGroup::SubGroupID(i));
            if (free_body_ids.empty()) {
                log::warn("physics: no room for a ragdoll");
                remove_bodies(r);
                return;
            }
            JPH::Body* body = bi.CreateBodyWithID(JPH::BodyID(free_body_ids.back()), c);
            free_body_ids.pop_back();
            bi.AddBody(body->GetID(), spec.asleep ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
            r.bodies.push_back(body->GetID());
            r.force_scale.push_back(b.force_scale);
        }
        const JPH::BodyLockInterfaceNoLock& locks = system.GetBodyLockInterfaceNoLock();
        for (const RagdollConstraintSpec& k : spec.constraints) {
            filter->DisableCollision(JPH::CollisionGroup::SubGroupID(k.body_a), JPH::CollisionGroup::SubGroupID(k.body_b));
            JPH::BodyLockWrite la(locks, r.bodies[size_t(k.body_a)]), lb(locks, r.bodies[size_t(k.body_b)]);
            if (!la.Succeeded() || !lb.Succeeded()) {
                continue;
            }
            JPH::Ref<JPH::ConstraintSettings> settings;
            if (k.hinge) {
                auto* h = new JPH::HingeConstraintSettings;
                h->mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
                const glm::vec3 com_a = com_shift(la.GetBody()), com_b = com_shift(lb.GetBody());
                h->mPoint1 = JPH::RVec3(to_jolt(k.pivot_b - com_b));
                h->mPoint2 = JPH::RVec3(to_jolt(k.pivot_a - com_a));
                h->mHingeAxis1 = to_jolt(k.axes_b[0]);
                h->mHingeAxis2 = to_jolt(k.axes_a[0]);
                h->mNormalAxis1 = to_jolt(k.axes_b[1]);
                h->mNormalAxis2 = to_jolt(k.axes_a[1]);
                h->mLimitsMin = k.hinge_min;
                h->mLimitsMax = k.hinge_max;
                h->mMaxFrictionTorque = k.friction;
                settings = h;
            } else {
                // Havok limits body A's twist axis against body B's frame, the plane limit asymmetrically: Jolt's
                // pyramid swing around y and z of body 1's frame, so B comes first.
                using Axis = JPH::SixDOFConstraintSettings::EAxis;
                auto* s = new JPH::SixDOFConstraintSettings;
                s->mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
                s->mPosition1 = JPH::RVec3(to_jolt(k.pivot_b - com_shift(lb.GetBody())));
                s->mPosition2 = JPH::RVec3(to_jolt(k.pivot_a - com_shift(la.GetBody())));
                s->mAxisX1 = to_jolt(k.axes_b[0]);
                s->mAxisY1 = to_jolt(k.axes_b[1]);
                s->mAxisX2 = to_jolt(k.axes_a[0]);
                s->mAxisY2 = to_jolt(k.axes_a[1]);
                s->MakeFixedAxis(Axis::TranslationX);
                s->MakeFixedAxis(Axis::TranslationY);
                s->MakeFixedAxis(Axis::TranslationZ);
                s->mSwingType = JPH::ESwingType::Pyramid;
                s->SetLimitedAxis(Axis::RotationX, k.twist_min, k.twist_max);
                s->SetLimitedAxis(Axis::RotationY, -k.cone_max, k.cone_max);
                s->SetLimitedAxis(Axis::RotationZ, std::max(k.plane_min, -k.cone_max), std::min(k.plane_max, k.cone_max));
                s->mMaxFriction[Axis::RotationX] = s->mMaxFriction[Axis::RotationY] = s->mMaxFriction[Axis::RotationZ] = k.friction;
                settings = s;
            }
            auto* two = static_cast<JPH::TwoBodyConstraintSettings*>(settings.GetPtr());
            r.constraints.push_back(two->Create(lb.GetBody(), la.GetBody()));
        }
        for (auto& c : r.constraints) {
            system.AddConstraint(c);
        }
        ragdolls.emplace(id, std::move(r));
    }

    // A capsule between two points, tagged with a collision layer. Jolt's stand on y; Havok's run between their ends.
    static JPH::RefConst<JPH::Shape> capsule(const glm::vec3& a, const glm::vec3& b, float radius, uint8_t layer) {
        const glm::vec3 axis = b - a;
        const float length = glm::length(axis);
        const glm::quat turn = length > 1e-5f ? glm::quat(glm::vec3(0, 1, 0), axis / length) : glm::quat(1, 0, 0, 0);
        JPH::Ref<JPH::Shape> leaf;
        if (length > 1e-5f) {
            leaf = new JPH::CapsuleShape(length * 0.5f, radius);
        } else {
            leaf = new JPH::SphereShape(radius);
        }
        leaf->SetUserData(shape_user_data(layer, 0));
        JPH::RotatedTranslatedShapeSettings settings(to_jolt((a + b) * 0.5f), JPH::Quat(turn.x, turn.y, turn.z, turn.w), leaf);
        return settings.Create().Get();
    }

    static glm::vec3 com_shift(const JPH::Body& body) {
        const JPH::Vec3 s = body.GetShape()->GetCenterOfMass();
        return {s.GetX(), s.GetY(), s.GetZ()};
    }

    void remove_ragdoll_now(uint32_t id) {
        auto it = ragdolls.find(id);
        if (it == ragdolls.end()) {
            return;
        }
        for (auto& c : it->second.constraints) {
            system.RemoveConstraint(c);
        }
        remove_bodies(it->second);
        ragdolls.erase(it);
    }

    void remove_bodies(const Ragdoll& r) {
        auto& bi = system.GetBodyInterface();
        for (JPH::BodyID b : r.bodies) {
            bi.RemoveBody(b);
            bi.DestroyBody(b);
            free_body_ids.push_back(b.GetIndex());
        }
    }

    // Keyframed: they move by what the animation says and push what they meet, as Havok's motion 4 does.
    void update_animated() {
        std::vector<uint32_t> remove;
        std::vector<AnimatedLoad> ready;
        std::unordered_map<uint32_t, std::vector<NodePose>> moves;
        {
            std::lock_guard lock(ragdoll_mutex);
            remove.swap(animated_to_remove);
            moves.swap(animated_moves);
            for (size_t i = 0; i < animated_to_add.size();) {
                if (animated_to_add[i].shapes.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    ready.push_back(std::move(animated_to_add[i]));
                    animated_to_add.erase(animated_to_add.begin() + static_cast<ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
        }
        auto& bi = system.GetBodyInterface();
        for (auto& [form, poses] : moves) {
            animated_poses[form] = std::move(poses);
        }
        std::vector<uint32_t> changed;
        for (const auto& [form, poses] : moves) {
            changed.push_back(form);
        }
        for (AnimatedLoad& load : ready) {
            Animated& a = animated[load.form_id];
            a.bodies = load.shapes.get();
            changed.push_back(load.form_id);
        }
        for (const uint32_t form : remove) {
            if (const auto it = animated.find(form); it != animated.end()) {
                for (const AnimatedBody& b : it->second.bodies) {
                    if (!b.id.IsInvalid()) {
                        animated_by_body.erase(b.id.GetIndex());
                        bi.RemoveBody(b.id);
                        bi.DestroyBody(b.id);
                        free_body_ids.push_back(b.id.GetIndex());
                    }
                }
                animated.erase(it);
            }
            animated_poses.erase(form);
        }
        // A keyframed body keeps the velocity of its last move: one the animation left has stopped.
        for (auto& [form, a] : animated) {
            if (a.moving && !moves.contains(form)) {
                a.moving = false;
                for (const AnimatedBody& b : a.bodies) {
                    if (!b.id.IsInvalid()) {
                        bi.SetLinearAndAngularVelocity(b.id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
                    }
                }
            }
        }
        for (const uint32_t form : changed) {
            const auto it = animated.find(form);
            const auto known = animated_poses.find(form);
            if (it == animated.end() || known == animated_poses.end()) {
                continue;
            }
            const std::vector<NodePose>& poses = known->second;
            it->second.moving = true;
            for (AnimatedBody& b : it->second.bodies) {
                const auto pose = std::ranges::find(poses, b.node, &NodePose::node);
                if (pose == poses.end()) {
                    continue;
                }
                const JPH::RVec3 at(pose->position.x, pose->position.y, pose->position.z);
                const JPH::Quat turn(pose->rotation.x, pose->rotation.y, pose->rotation.z, pose->rotation.w);
                if (b.id.IsInvalid()) {
                    if (free_body_ids.empty()) {
                        continue;
                    }
                    JPH::BodyCreationSettings c(b.shape, at, turn.Normalized(), JPH::EMotionType::Kinematic, object_layer::moving);
                    JPH::Body* body = bi.CreateBodyWithID(JPH::BodyID(free_body_ids.back()), c);
                    free_body_ids.pop_back();
                    b.id = body->GetID();
                    bi.AddBody(b.id, JPH::EActivation::Activate);
                    animated_by_body[b.id.GetIndex()] = form;
                } else {
                    bi.MoveKinematic(b.id, at, turn.Normalized(), 1.0f / 60.0f);
                }
            }
        }
    }

    void update_objects() {
        std::vector<uint32_t> remove;
        std::vector<Object> ready;
        {
            std::lock_guard lock(ragdoll_mutex);
            remove.swap(objects_to_remove);
            for (size_t i = 0; i < objects_to_add.size();) {
                Object& o = objects_to_add[i];
                if (std::ranges::find(remove, o.form_id) != remove.end()) {
                    objects_to_add.erase(objects_to_add.begin() + static_cast<ptrdiff_t>(i));
                } else if (o.shape.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    ready.push_back(std::move(o));
                    objects_to_add.erase(objects_to_add.begin() + static_cast<ptrdiff_t>(i));
                } else {
                    ++i;
                }
            }
        }
        auto& bi = system.GetBodyInterface();
        for (const uint32_t form : remove) {
            if (const auto it = objects.find(form); it != objects.end()) {
                bi.RemoveBody(it->second);
                bi.DestroyBody(it->second);
                free_body_ids.push_back(it->second.GetIndex());
                objects.erase(it);
            }
        }
        for (Object& o : ready) {
            const Built built = o.shape.get();
            if (built.shape && !built.dynamic && o.fixed && !free_body_ids.empty()) {
                JPH::BodyCreationSettings c(built.shape, JPH::RVec3(o.position.x, o.position.y, o.position.z),
                                            JPH::Quat(o.rotation.x, o.rotation.y, o.rotation.z, o.rotation.w),
                                            JPH::EMotionType::Static, object_layer::fixed);
                JPH::Body* body = bi.CreateBodyWithID(JPH::BodyID(free_body_ids.back()), c);
                free_body_ids.pop_back();
                bi.AddBody(body->GetID(), JPH::EActivation::DontActivate);
                objects[o.form_id] = body->GetID();
                continue;
            }
            if (!built.shape || !built.dynamic || free_body_ids.empty()) {
                // No body to fall with: it rests on what is under it.
                JPH::RRayCast ray(JPH::RVec3(o.position.x, o.position.y, o.position.z), JPH::Vec3(0, 0, -k_object_rest_reach));
                JPH::RayCastResult hit;
                if (system.GetNarrowPhaseQueryNoLock().CastRay(ray, hit, k_no_actors)) {
                    std::lock_guard lock(ragdoll_mutex);
                    o.position.z -= hit.mFraction * k_object_rest_reach;
                    moved_since_read.push_back({o.form_id, o.position, o.rotation});
                }
                continue;
            }
            JPH::BodyCreationSettings c(built.shape, JPH::RVec3(o.position.x, o.position.y, o.position.z),
                                        JPH::Quat(o.rotation.x, o.rotation.y, o.rotation.z, o.rotation.w),
                                        JPH::EMotionType::Dynamic, object_layer::moving);
            c.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            c.mMassPropertiesOverride.mMass = built.mass;
            c.mMotionQuality = JPH::EMotionQuality::LinearCast; // Havok's moving quality: continuous against the world
            c.mFriction = built.friction;
            c.mRestitution = built.restitution;
            JPH::Body* body = bi.CreateBodyWithID(JPH::BodyID(free_body_ids.back()), c);
            free_body_ids.pop_back();
            bi.AddBody(body->GetID(), JPH::EActivation::Activate);
            objects[o.form_id] = body->GetID();
        }
    }

    void update_ragdolls_before() {
        update_objects();
        update_animated();
        std::vector<std::pair<uint32_t, RagdollSpec>> add;
        std::vector<uint32_t> remove;
        std::vector<RagdollPush> push;
        std::vector<std::pair<uint32_t, glm::vec3>> impulses;
        std::vector<std::pair<uint32_t, glm::vec3>> steers;
        {
            std::lock_guard lock(ragdoll_mutex);
            add.swap(ragdolls_to_add);
            remove.swap(ragdolls_to_remove);
            push.swap(ragdolls_to_push);
            impulses.swap(ref_impulses);
            steers.swap(ref_steers);
        }
        for (auto& entry : add) {
            // Over ground that hasn't streamed in it would fall for good: it waits.
            if (entry.second.bodies.empty() || ready(entry.second.bodies[0].position)) {
                add_ragdoll_now(entry.first, entry.second);
            } else {
                std::lock_guard lock(ragdoll_mutex);
                ragdolls_to_add.push_back(std::move(entry));
            }
        }
        for (uint32_t id : remove) {
            remove_ragdoll_now(id);
            std::lock_guard lock(ragdoll_mutex);
            ragdoll_states.erase(id);
        }
        auto& bi = system.GetBodyInterface();
        for (const auto& [ref, impulse] : impulses) {
            const JPH::BodyID body(uint32_t(world->cells.size()) + ref);
            if (bi.IsAdded(body)) {
                bi.AddImpulse(body, to_jolt(impulse));
            }
        }
        for (const auto& [ref, velocity] : steers) {
            const JPH::BodyID body(uint32_t(world->cells.size()) + ref);
            if (bi.IsAdded(body)) {
                bi.SetLinearVelocity(body, to_jolt(velocity));
                bi.ActivateBody(body);
            }
        }
        for (const RagdollPush& p : push) {
            const auto it = ragdolls.find(p.id);
            if (it == ragdolls.end()) {
                continue;
            }
            for (size_t i = 0; i < it->second.bodies.size(); ++i) {
                const JPH::BodyID body = it->second.bodies[i];
                const glm::vec3 away = to_glm(JPH::Vec3(bi.GetPosition(body))) - p.centre;
                const float distance = glm::length(away);
                if (distance > 1e-5f) {
                    bi.AddLinearVelocity(body, to_jolt(away / distance * (p.strength * it->second.force_scale[i])));
                }
            }
        }
    }

    void update_ragdolls_after() {
        auto& bi = system.GetBodyInterface();
        std::lock_guard lock(ragdoll_mutex);
        JPH::BodyIDVector active;
        system.GetActiveBodies(JPH::EBodyType::RigidBody, active);
        const uint32_t first_ref = uint32_t(world->cells.size()), end_ref = first_ref + uint32_t(world->refs.size());
        for (const auto& [form, id] : objects) {
            if (bi.IsActive(id)) {
                JPH::RVec3 p;
                JPH::Quat q;
                bi.GetPositionAndRotation(id, p, q);
                moved_since_read.push_back({form, glm::vec3(float(p.GetX()), float(p.GetY()), float(p.GetZ())),
                                            glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ())});
            }
        }
        for (const JPH::BodyID id : active) {
            const uint32_t index = id.GetIndex();
            if (index < first_ref || index >= end_ref) {
                continue;
            }
            JPH::RVec3 p;
            JPH::Quat q;
            bi.GetPositionAndRotation(id, p, q);
            const glm::vec3 position(float(p.GetX()), float(p.GetY()), float(p.GetZ()));
            const glm::quat rotation(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
            RagdollBodyState& state = moved_refs[index - first_ref];
            if (state.position != position || state.rotation != rotation) {
                state = {position, rotation};
                moved_since_read.push_back({world->refs[index - first_ref].form_id, position, rotation});
            }
        }
        for (const auto& [id, r] : ragdolls) {
            auto& state = ragdoll_states[id];
            state.resize(r.bodies.size());
            for (size_t i = 0; i < r.bodies.size(); ++i) {
                JPH::RVec3 p;
                JPH::Quat q;
                bi.GetPositionAndRotation(r.bodies[i], p, q);
                state[i].position = {float(p.GetX()), float(p.GetY()), float(p.GetZ())};
                state[i].rotation = glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
            }
        }
    }
    JPH::PhysicsSystem system;
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<JPH::JobSystemSingleThreaded> jobs;

    std::vector<int32_t> grid;
    int32_t grid_min[2] = {0, 0};
    int32_t grid_size[2] = {0, 0};
    std::vector<CellSlot> cells;
    std::vector<uint32_t> active; // cells not in state none
    // Written by the simulation thread, read by stats() and busy() from the main thread.
    std::atomic<uint32_t> loaded{0};
    std::atomic<uint32_t> requested{0};
    std::atomic<uint32_t> bodies{0};
    bool broad_phase_dirty = false;
    glm::vec3 focus{1e30f};
    // References that don't collide (open doors), by index into the world's refs: their bodies stay out of the
    // system. Changes come from any thread and are applied by update().
    std::mutex solid_mutex;
    std::once_flag ref_order_once;
    std::vector<std::pair<uint32_t, uint32_t>> ref_order; // (form id, index), sorted
    std::vector<std::pair<uint32_t, bool>> solid_changes;
    std::unordered_set<uint32_t> not_solid;
    std::vector<World::Leaf> leaves;  // the next update's, under solid_mutex
    bool leaves_changed = false;
    glm::vec3 leaf_half[World::k_max_leaves] = {}; // the bodies there are, after the primitives'; zero: none
    std::vector<World::ActorBody> actor_bodies; // the next update's, under solid_mutex
    bool actor_bodies_changed = false;
    struct ActorSlot {
        uint32_t form = 0; // 0: no body
        glm::vec3 half{0.0f};
        float height = 0.0f; // of its middle over the feet
    };
    ActorSlot actor_slots[World::k_max_actors]; // the bodies there are, after the leaves'
    // Impulses from the game's thread, applied by update().
    struct Push {
        uint32_t body;
        glm::vec3 linear, angular, at;
    };
    std::vector<Push> pushes;

    // Shared with loaders.
    std::mutex model_mutex;
    std::condition_variable model_cv;
    std::vector<Model> models;
    uint32_t models_alive = 0;
    uint32_t models_missing = 0;
    uint64_t model_bytes = 0;

    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<std::pair<float, uint32_t>> queue; // (priority, cell), nearest first
    std::vector<Prepared> done;
    bool stop = false;
    std::vector<std::thread> loaders;

    Impl(const std::filesystem::path& root, std::shared_ptr<const osw::World> w, const WorldSettings& s)
        : cache_root(root), world(std::move(w)), settings(s) {
        init_jolt();
        const auto& wd = *world;
        // Fixed body IDs: terrain of cell i is body i, reference r is body cells + r.
        const uint32_t world_bodies = static_cast<uint32_t>(wd.cells.size() + wd.refs.size() + wd.primitives.size() + World::k_max_leaves + 1 + World::k_max_actors);
        const uint32_t max_bodies = world_bodies + k_ragdoll_bodies;
        for (uint32_t id = max_bodies; id > world_bodies; --id) {
            free_body_ids.push_back(id - 1);
        }
        system.Init(max_bodies, 0, 1024, 1024, broad_phase_layers, object_vs_broad_phase, object_pairs);
        system.SetGravity(JPH::Vec3(0, 0, -9.81f));
        // Havok takes the geometric mean of two materials' restitution; Jolt's default is the larger.
        system.SetCombineRestitution([](const JPH::Body& a, const JPH::SubShapeID&, const JPH::Body& b, const JPH::SubShapeID&) {
            return std::sqrt(a.GetRestitution() * b.GetRestitution());
        });
        temp = std::make_unique<JPH::TempAllocatorImpl>(4u << 20);
        jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);

        int32_t lo[2] = {INT32_MAX, INT32_MAX}, hi[2] = {INT32_MIN, INT32_MIN};
        for (const auto& c : wd.cells) {
            lo[0] = std::min(lo[0], c.x);
            lo[1] = std::min(lo[1], c.y);
            hi[0] = std::max(hi[0], c.x);
            hi[1] = std::max(hi[1], c.y);
        }
        if (!wd.cells.empty()) {
            grid_min[0] = lo[0];
            grid_min[1] = lo[1];
            grid_size[0] = hi[0] - lo[0] + 1;
            grid_size[1] = hi[1] - lo[1] + 1;
            grid.assign(size_t(grid_size[0]) * grid_size[1], -1);
            for (uint32_t i = 0; i < wd.cells.size(); ++i) {
                grid[size_t(wd.cells[i].y - lo[1]) * grid_size[0] + (wd.cells[i].x - lo[0])] = static_cast<int32_t>(i);
            }
        }
        cells.resize(wd.cells.size());
        models.resize(wd.meshes.size());
        for (unsigned t = 0; t < std::max(1u, settings.loader_threads); ++t) {
            loaders.emplace_back([this] { loader_main(); });
        }
    }

    ~Impl() {
        {
            std::lock_guard lock(queue_mutex);
            stop = true;
        }
        queue_cv.notify_all();
        for (auto& t : loaders) {
            t.join();
        }
        auto& bi = system.GetBodyInterfaceNoLock();
        for (auto& p : done) {
            bi.AddBodiesAbort(p.bodies.data(), static_cast<int>(p.bodies.size()), p.add_state);
            bi.DestroyBodies(p.bodies.data(), static_cast<int>(p.bodies.size()));
        }
        for (auto& c : cells) {
            if (!c.bodies.empty()) {
                remove_bodies(c.bodies);
                bi.DestroyBodies(c.bodies.data(), static_cast<int>(c.bodies.size()));
            }
        }
    }

    int32_t lookup(int32_t x, int32_t y) const {
        const int32_t gx = x - grid_min[0], gy = y - grid_min[1];
        if (gx < 0 || gy < 0 || gx >= grid_size[0] || gy >= grid_size[1]) {
            return -1;
        }
        return grid[size_t(gy) * grid_size[0] + gx];
    }

    // Distance from p to the cell's square in the ground plane.
    float cell_distance(uint32_t ci, const glm::vec3& p) const {
        const auto& c = world->cells[ci];
        const float cs = world->cell_size;
        const float dx = std::max({c.x * cs - p.x, 0.0f, p.x - (c.x + 1) * cs});
        const float dy = std::max({c.y * cs - p.y, 0.0f, p.y - (c.y + 1) * cs});
        return std::sqrt(dx * dx + dy * dy);
    }

    template <typename Fn>
    void cells_near(const glm::vec3& p, float radius, Fn&& fn) const {
        const float cs = world->cell_size;
        const int32_t x0 = static_cast<int32_t>(std::floor((p.x - radius) / cs));
        const int32_t x1 = static_cast<int32_t>(std::floor((p.x + radius) / cs));
        const int32_t y0 = static_cast<int32_t>(std::floor((p.y - radius) / cs));
        const int32_t y1 = static_cast<int32_t>(std::floor((p.y + radius) / cs));
        for (int32_t y = y0; y <= y1; ++y) {
            for (int32_t x = x0; x <= x1; ++x) {
                const int32_t ci = lookup(x, y);
                if (ci >= 0 && cell_distance(static_cast<uint32_t>(ci), p) <= radius) {
                    fn(static_cast<uint32_t>(ci));
                }
            }
        }
    }

    // ---- model shapes (loader threads; release from the sim thread) ----------------------------------------

    // Returns the model's shape (null if it has nothing solid), loading it if needed. Takes a reference.
    JPH::RefConst<JPH::Shape> acquire_model(uint32_t mi) {
        std::unique_lock lock(model_mutex);
        Model& m = models[mi];
        ++m.refs;
        model_cv.wait(lock, [&] { return m.state != Model::State::loading; });
        if (m.state == Model::State::ready) {
            return m.shape;
        }
        m.state = Model::State::loading;
        lock.unlock();

        Built built;
        bool missing = false;
        const auto file = cache_root / path::replace_extension(world->meshes[mi], ".osp");
        std::error_code ec;
        if (std::filesystem::exists(file, ec)) {
            try {
                built = build_shape(osp::read(read_file(file)), settings.character_layers | k_trap_layers);
                if (built.dropped > 0) {
                    log::debug("physics: {}: {} shapes could not be built", world->meshes[mi], built.dropped);
                }
            } catch (const std::exception& e) {
                log::warn("physics: {}: {}", file.string(), e.what());
            }
        } else {
            missing = true;
        }

        lock.lock();
        m.state = Model::State::ready;
        m.shape = built.shape;
        m.bytes = built.bytes;
        m.dynamic = built.dynamic;
        m.mass = built.mass;
        m.friction = built.friction;
        m.restitution = built.restitution;
        m.missing = missing || !built.shape;
        if (m.shape) {
            ++models_alive;
            model_bytes += m.bytes;
        } else {
            ++models_missing;
        }
        model_cv.notify_all();
        return m.shape;
    }

    void release_model(uint32_t mi) {
        std::lock_guard lock(model_mutex);
        Model& m = models[mi];
        if (m.refs > 0 && --m.refs == 0 && m.state == Model::State::ready) {
            if (m.shape) {
                --models_alive;
                model_bytes -= m.bytes;
            } else {
                --models_missing;
            }
            m.shape = nullptr;
            m.state = Model::State::none;
        }
    }

    // ---- loaders ---------------------------------------------------------------------------------------------

    void loader_main() {
        for (;;) {
            uint32_t ci;
            {
                std::unique_lock lock(queue_mutex);
                queue_cv.wait(lock, [&] { return stop || !queue.empty(); });
                if (stop) {
                    return;
                }
                ci = queue.front().second;
                queue.pop_front();
            }
            Prepared p = prepare_cell(ci);
            std::lock_guard lock(queue_mutex);
            done.push_back(std::move(p));
        }
    }

    Prepared prepare_cell(uint32_t ci) {
        Prepared p;
        p.cell = ci;
        const auto& w = *world;
        const auto& c = w.cells[ci];
        auto& bi = system.GetBodyInterface();
        // Terrain: the LAND's triangles, every quad split from its south-west to its north-east corner as the
        // game's land collision does, at the exact heights: a Jolt heightfield
        // stores 8 bits per sample, which on steep land moves heights by over half a unit and tilts the normals.
        if (c.land != osw::k_none && c.land < w.lands.size()) {
            constexpr int n = osw::k_land_verts;
            const auto& land = w.lands[c.land];
            const float spacing = w.cell_size / float(n - 1);
            // The game builds the land's heightfield 0.0125 Havok units below the LAND heights and gives its
            // triangles a 0.005 convex radius.
            JPH::VertexList verts;
            verts.reserve(size_t(n) * n);
            for (int north = 0; north < n; ++north) {
                for (int east = 0; east < n; ++east) {
                    verts.push_back(JPH::Float3(float(east) * spacing, float(north) * spacing,
                                                land.heights[size_t(north) * n + east] - k_terrain_drop));
                }
            }
            JPH::IndexedTriangleList tris;
            tris.reserve(size_t(n - 1) * (n - 1) * 2);
            for (int north = 0; north + 1 < n; ++north) {
                for (int east = 0; east + 1 < n; ++east) {
                    const uint32_t sw = uint32_t(north * n + east), se = sw + 1, nw = sw + n, ne = nw + 1;
                    tris.push_back(JPH::IndexedTriangle(sw, se, ne));
                    tris.push_back(JPH::IndexedTriangle(sw, ne, nw));
                }
            }
            auto result = JPH::MeshShapeSettings(std::move(verts), std::move(tris)).Create();
            if (result.HasError()) {
                log::warn("physics: terrain of cell ({}, {}): {}", c.x, c.y, result.GetError().c_str());
            } else {
                JPH::Ref<JPH::Shape> shape = result.Get();
                shape->SetUserData(shape_user_data(k_land_layer, 0, k_terrain_radius));
                JPH::BodyCreationSettings bcs(shape, JPH::RVec3(c.x * w.cell_size, c.y * w.cell_size, 0.0f),
                                              JPH::Quat::sIdentity(), JPH::EMotionType::Static, object_layer::fixed);
                bcs.mUserData = ci;
                if (JPH::Body* b = bi.CreateBodyWithID(JPH::BodyID(ci), bcs)) {
                    p.bodies.push_back(b->GetID());
                }
            }
        }
        const uint64_t end = std::min<uint64_t>(uint64_t(c.first_ref) + c.ref_count, w.refs.size());
        for (uint64_t ri = c.first_ref; ri < end; ++ri) {
            const auto& ref = w.refs[ri];
            if (ref.mesh >= models.size() || disabled(ref.form_id, ref.flags)) {
                continue; // disabled references are hidden and don't collide
            }
            JPH::RefConst<JPH::Shape> shape = acquire_model(ref.mesh);
            p.models.push_back(ref.mesh);
            if (!shape) {
                continue;
            }
            if (std::abs(ref.scale - 1.0f) > 1e-4f && ref.scale > 0.0f) {
                shape = new JPH::ScaledShape(shape, JPH::Vec3::sReplicate(ref.scale));
            }
            bool dynamic = false;
            float mass = 0, friction = 0.5f, restitution = 0.4f;
            {
                std::lock_guard lock(model_mutex);
                dynamic = models[ref.mesh].dynamic;
                mass = models[ref.mesh].mass;
                friction = models[ref.mesh].friction;
                restitution = models[ref.mesh].restitution;
            }
            JPH::RVec3 at(ref.position[0], ref.position[1], ref.position[2]);
            JPH::Quat turned = to_quat(ref.rotation);
            if (dynamic) {
                std::lock_guard lock(ragdoll_mutex);
                if (const auto moved = moved_refs.find(ri); moved != moved_refs.end()) {
                    const RagdollBodyState& s = moved->second;
                    at = JPH::RVec3(s.position.x, s.position.y, s.position.z);
                    turned = JPH::Quat(s.rotation.x, s.rotation.y, s.rotation.z, s.rotation.w);
                }
            }
            JPH::BodyCreationSettings bcs(shape, at, turned,
                                          dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
                                          dynamic ? object_layer::moving : object_layer::fixed);
            if (dynamic) {
                // Clutter is simulated from the moment its cell loads, settles where it was placed and sleeps
                // (measured: collision4, a deer skull rests 1.4 units below its placed height before the test).
                bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                bcs.mMassPropertiesOverride.mMass = mass;
                bcs.mMotionQuality = JPH::EMotionQuality::LinearCast;
                bcs.mFriction = friction;
                bcs.mRestitution = restitution;
                bcs.mIsSensor = false;
                bcs.mAllowSleeping = true;
            }
            bcs.mUserData = ri;
            const uint32_t id = static_cast<uint32_t>(w.cells.size() + ri);
            if (JPH::Body* b = bi.CreateBodyWithID(JPH::BodyID(id), bcs)) {
                p.bodies.push_back(b->GetID());
                if (dynamic) {
                    p.dynamic_bodies.push_back(b->GetID());
                }
            }
        }
        // CollisionMarker primitives: fixed boxes, spheres and thin boxes for planes, with Havok's 0.05 shell on
        // the boxes (bhkBoxShape takes g_HavokConvexRadius), on their own layer.
        auto first = std::ranges::lower_bound(w.primitives, std::pair(c.y, c.x), {},
                                              [](const osw::Primitive& q) { return std::pair(q.cell_y, q.cell_x); });
        for (auto it = first; it != w.primitives.end() && it->cell_x == c.x && it->cell_y == c.y; ++it) {
            const osw::Primitive& q = *it;
            if (disabled(q.form_id, q.flags) || q.layer >= 64 ||
                !((settings.character_layers >> q.layer) & 1)) {
                continue;
            }
            JPH::ShapeSettings::ShapeResult result;
            float shell = k_box_shell;
            if (q.type == osw::PrimitiveType::sphere) {
                result = JPH::SphereShapeSettings(std::max(q.half_extents[0], 0.001f)).Create();
                shell = 0.0f;
            } else {
                const float u = 0.0142875f;
                JPH::Vec3 half(q.half_extents[0], q.half_extents[1], q.half_extents[2]);
                if (q.type == osw::PrimitiveType::plane) {
                    half = JPH::Vec3(q.half_extents[0] < 0.001f * u ? u : q.half_extents[0], 0.00014287f,
                                     q.half_extents[2] < 0.001f * u ? u : q.half_extents[2]);
                }
                result = JPH::BoxShapeSettings(JPH::Vec3::sMax(half, JPH::Vec3::sReplicate(0.0001f)), 0.0f).Create();
            }
            if (result.HasError()) {
                continue;
            }
            JPH::Ref<JPH::Shape> shape = result.Get();
            shape->SetUserData(shape_user_data(static_cast<uint8_t>(q.layer), 0, shell));
            JPH::BodyCreationSettings bcs(shape, JPH::RVec3(q.position[0], q.position[1], q.position[2]),
                                          to_quat(q.rotation), JPH::EMotionType::Static, object_layer::fixed);
            const auto index = static_cast<uint32_t>(it - w.primitives.begin());
            bcs.mUserData = index;
            const uint32_t id = static_cast<uint32_t>(w.cells.size() + w.refs.size() + index);
            if (JPH::Body* b = bi.CreateBodyWithID(JPH::BodyID(id), bcs)) {
                p.bodies.push_back(b->GetID());
            }
        }
        if (!p.bodies.empty()) {
            p.add_state = bi.AddBodiesPrepare(p.bodies.data(), static_cast<int>(p.bodies.size()));
        }
        return p;
    }

    // ---- sim thread ------------------------------------------------------------------------------------------

    void request(uint32_t ci, float priority) {
        auto& slot = cells[ci];
        if (slot.state == CellState::none) {
            active.push_back(ci);
        }
        slot.state = CellState::requested;
        slot.cancelled = false;
        ++requested;
        std::lock_guard lock(queue_mutex);
        auto it = std::ranges::upper_bound(queue, priority, {}, &std::pair<float, uint32_t>::first);
        queue.insert(it, {priority, ci});
        queue_cv.notify_one();
    }

    // Takes out the bodies of references that don't collide (they stay created, for when they do again).
    void remove_not_solid(const std::vector<JPH::BodyID>& ids) {
        if (not_solid.empty()) {
            return;
        }
        auto& bi = system.GetBodyInterfaceNoLock();
        const uint32_t first_ref = static_cast<uint32_t>(world->cells.size());
        for (const JPH::BodyID id : ids) {
            const uint32_t n = id.GetIndex();
            if (n >= first_ref && not_solid.contains(n - first_ref) && bi.IsAdded(id)) {
                bi.RemoveBody(id);
                broad_phase_dirty = true;
            }
        }
    }

    void apply_leaves() {
        std::vector<World::Leaf> now;
        {
            std::lock_guard lock(solid_mutex);
            if (!leaves_changed) {
                return;
            }
            leaves_changed = false;
            now = leaves;
        }
        auto& bi = system.GetBodyInterfaceNoLock();
        const auto first = static_cast<uint32_t>(world->cells.size() + world->refs.size() + world->primitives.size());
        for (uint32_t i = 0; i < World::k_max_leaves; ++i) {
            const JPH::BodyID id(first + i);
            const bool wanted = i < now.size();
            if (leaf_half[i] != glm::vec3(0.0f) && (!wanted || now[i].half != leaf_half[i])) {
                bi.RemoveBody(id);
                bi.DestroyBody(id);
                leaf_half[i] = glm::vec3(0.0f);
                broad_phase_dirty = true;
            }
            if (!wanted) {
                continue;
            }
            const World::Leaf& l = now[i];
            const JPH::RVec3 at(l.centre.x, l.centre.y, l.centre.z);
            const JPH::Quat turn(l.rotation.x, l.rotation.y, l.rotation.z, l.rotation.w);
            if (leaf_half[i] == glm::vec3(0.0f)) {
                const JPH::Vec3 half = JPH::Vec3::sMax(JPH::Vec3(l.half.x, l.half.y, l.half.z), JPH::Vec3::sReplicate(0.0001f));
                JPH::ShapeSettings::ShapeResult result = JPH::BoxShapeSettings(half, 0.0f).Create();
                if (result.HasError()) {
                    continue;
                }
                JPH::Ref<JPH::Shape> shape = result.Get();
                shape->SetUserData(shape_user_data(k_leaf_layer, 0, 0.0f));
                // Keyframed, as an animated object: the character proxy takes its velocity at the contact, so a swinging
                // leaf pushes whoever stands in its way.
                JPH::BodyCreationSettings bcs(shape, at, turn, JPH::EMotionType::Kinematic, object_layer::moving);
                if (JPH::Body* b = bi.CreateBodyWithID(id, bcs)) {
                    bi.AddBody(b->GetID(), JPH::EActivation::Activate);
                    leaf_half[i] = l.half;
                    broad_phase_dirty = true;
                }
            } else if ((bi.GetPosition(id) - at).Length() > k_leaf_jump_m) {
                // Another door's leaf took this body when the list shifted: that is no swing.
                bi.SetPositionAndRotation(id, at, turn, JPH::EActivation::Activate);
                broad_phase_dirty = true;
            } else {
                bi.MoveKinematic(id, at, turn, 1.0f / 60.0f);
            }
        }
    }

    // a bound taller than it is long or wide makes the hull the player has; a
    // longer one a capsule along its long axis, or three for a four-legged body.
    static JPH::Ref<JPH::Shape> actor_shape(const World::ActorBody& a, float& height) {
        const glm::vec3 h = a.half_extents;
        if (h.x <= h.z && h.y <= h.z) {
            const float r = 0.5f * (h.x + h.y);
            height = character_hull_centre(h);
            JPH::Ref<JPH::Shape> hull = character_hull(h, std::max(k_actor_step_m * a.scale, 0.75f * r));
            hull->SetUserData(shape_user_data(k_charcontroller, 0, k_actor_shell_m));
            return hull;
        }
        // The capsules' ends start from the bound's centre, and the whole shape stands there again.
        const glm::vec2 centre = a.centre;
        const int along = h.x > h.y ? 0 : 1;
        const float wide = h[1 - along];
        const float radius = std::min(wide, h.z), large = std::max(wide, h.z);
        glm::vec3 front(centre, 0.0f);
        front[along] += h[along] - radius;
        glm::vec3 back = front;
        back[along] = -back[along];
        const glm::vec3 place(centre, 0.0f);
        JPH::StaticCompoundShapeSettings parts;
        const auto add = [&](glm::vec3 from, glm::vec3 to, float thick) {
            from += place;
            to += place;
            const glm::vec3 d = to - from;
            const float length = glm::length(d);
            JPH::Ref<JPH::Shape> capsule = new JPH::CapsuleShape(std::max(0.5f * length, 0.001f), thick); // along Jolt's Y
            capsule->SetUserData(shape_user_data(k_charcontroller, 0, 0.0f));
            const JPH::Vec3 axis = length > 0.0f ? to_jolt(d / length) : JPH::Vec3::sAxisY();
            parts.AddShape(to_jolt(0.5f * (from + to)), JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), axis), capsule);
        };
        if (radius >= 0.75f * large) {
            height = radius + 0.1f;
            add(front, back, radius);
        } else {
            // Too thin for its height: a body on top, as high as the bound, on a ball at each end reaching the ground, 8
            // units in along Y.
            constexpr float k_leg_in = 8.0f * 0.0142875f, k_leg = 1.0f * 0.0142875f;
            const float rise = large - radius;
            height = 0.05f;
            front.z = back.z = rise + large;
            add(front, back, radius);
            const glm::vec3 fore = front - glm::vec3(0.0f, k_leg_in, 0.0f), aft = back + glm::vec3(0.0f, k_leg_in, 0.0f);
            add(glm::vec3(fore.x, fore.y, rise + k_leg), glm::vec3(fore.x, fore.y, rise - k_leg), rise);
            add(glm::vec3(aft.x, aft.y, rise + k_leg), glm::vec3(aft.x, aft.y, rise - k_leg), rise);
        }
        return parts.Create().Get();
    }

    // Other actors' controllers: the hull the player has, on L_CHARCONTROLLER, whose row holds itself (
    // "Other actors"). Keyframed, so the proxy takes one's walk as it does a door's swing.
    void apply_actor_bodies() {
        std::vector<World::ActorBody> now;
        {
            std::lock_guard lock(solid_mutex);
            if (!actor_bodies_changed) {
                return;
            }
            actor_bodies_changed = false;
            now = actor_bodies;
        }
        auto& bi = system.GetBodyInterfaceNoLock();
        const auto first = static_cast<uint32_t>(world->cells.size() + world->refs.size() + world->primitives.size() +
                                                 World::k_max_leaves + 1);
        for (uint32_t i = 0; i < World::k_max_actors; ++i) {
            ActorSlot& slot = actor_slots[i];
            const auto wanted = std::ranges::find_if(now, [&](const World::ActorBody& a) {
                return a.form_id == slot.form && a.half_extents == slot.half;
            });
            if (slot.form && wanted == now.end()) {
                bi.RemoveBody(JPH::BodyID(first + i));
                bi.DestroyBody(JPH::BodyID(first + i));
                slot = {};
                broad_phase_dirty = true;
            }
        }
        for (const World::ActorBody& a : now) {
            const JPH::Quat turn = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), -a.heading);
            ActorSlot* slot = nullptr;
            for (ActorSlot& s : actor_slots) {
                if (s.form == a.form_id) {
                    slot = &s;
                }
            }
            if (slot) {
                const glm::vec3 centre = a.feet + glm::vec3(0.0f, 0.0f, slot->height);
                const JPH::RVec3 at(centre.x, centre.y, centre.z);
                const JPH::BodyID id(first + static_cast<uint32_t>(slot - actor_slots));
                if ((bi.GetPosition(id) - at).Length() > k_actor_jump_m) {
                    bi.SetPositionAndRotation(id, at, turn, JPH::EActivation::Activate);
                    broad_phase_dirty = true;
                } else {
                    bi.MoveKinematic(id, at, turn, 1.0f / 60.0f);
                }
                continue;
            }
            for (ActorSlot& s : actor_slots) {
                if (s.form) {
                    continue;
                }
                float height = 0.0f;
                JPH::Ref<JPH::Shape> shape = actor_shape(a, height);
                const glm::vec3 centre = a.feet + glm::vec3(0.0f, 0.0f, height);
                const JPH::RVec3 at(centre.x, centre.y, centre.z);
                JPH::BodyCreationSettings bcs(shape, at, turn, JPH::EMotionType::Kinematic, object_layer::actor);
                const JPH::BodyID id(first + static_cast<uint32_t>(&s - actor_slots));
                if (JPH::Body* b = bi.CreateBodyWithID(id, bcs)) {
                    bi.AddBody(b->GetID(), JPH::EActivation::Activate);
                    s = {a.form_id, a.half_extents, height};
                    broad_phase_dirty = true;
                }
                break;
            }
        }
    }

    void apply_solid_changes() {
        std::vector<std::pair<uint32_t, bool>> changes;
        {
            std::lock_guard lock(solid_mutex);
            changes.swap(solid_changes);
        }
        auto& bi = system.GetBodyInterfaceNoLock();
        for (const auto& [ri, solid] : changes) {
            const JPH::BodyID id(static_cast<uint32_t>(world->cells.size() + ri));
            bool exists = false;
            {
                JPH::BodyLockRead lock(system.GetBodyLockInterfaceNoLock(), id);
                exists = lock.Succeeded();
            }
            if (solid) {
                not_solid.erase(ri);
                if (exists && !bi.IsAdded(id)) {
                    bi.AddBody(id, JPH::EActivation::DontActivate);
                    broad_phase_dirty = true;
                }
            } else {
                not_solid.insert(ri);
                if (exists && bi.IsAdded(id)) {
                    bi.RemoveBody(id);
                    broad_phase_dirty = true;
                }
            }
        }
    }

    void remove_bodies(std::vector<JPH::BodyID>& ids) {
        auto& bi = system.GetBodyInterfaceNoLock();
        if (!not_solid.empty()) {
            std::vector<JPH::BodyID> added;
            for (const JPH::BodyID id : ids) {
                if (bi.IsAdded(id)) {
                    added.push_back(id);
                }
            }
            if (!added.empty()) {
                bi.RemoveBodies(added.data(), static_cast<int>(added.size()));
            }
        } else {
            bi.RemoveBodies(ids.data(), static_cast<int>(ids.size()));
        }
    }

    void unload(uint32_t ci) {
        auto& slot = cells[ci];
        auto& bi = system.GetBodyInterfaceNoLock();
        if (!slot.bodies.empty()) {
            remove_bodies(slot.bodies);
            bi.DestroyBodies(slot.bodies.data(), static_cast<int>(slot.bodies.size()));
            bodies -= static_cast<uint32_t>(slot.bodies.size());
            broad_phase_dirty = true;
        }
        for (uint32_t mi : slot.models) {
            release_model(mi);
        }
        slot.bodies.clear();
        slot.models.clear();
        slot.state = CellState::none;
        --loaded;
    }

    void update(const glm::vec3& p) {
        const auto start = Clock::now();
        focus = p;
        apply_solid_changes();
        apply_leaves();
        apply_actor_bodies();
        // Requests, nearest first.
        cells_near(p, settings.load_radius, [&](uint32_t ci) {
            auto& slot = cells[ci];
            if (slot.state == CellState::none) {
                request(ci, cell_distance(ci, p));
            } else if (slot.state == CellState::requested) {
                slot.cancelled = false;
            }
        });
        // Finished loads.
        std::vector<Prepared> ready;
        {
            std::lock_guard lock(queue_mutex);
            ready.swap(done);
        }
        auto& bi = system.GetBodyInterfaceNoLock();
        size_t i = 0;
        for (; i < ready.size(); ++i) {
            if (i > 0 && ms_since(start) > settings.budget_ms) {
                break;
            }
            auto& r = ready[i];
            auto& slot = cells[r.cell];
            --requested;
            if (slot.cancelled || cell_distance(r.cell, p) > settings.unload_radius) {
                if (!r.bodies.empty()) {
                    bi.AddBodiesAbort(r.bodies.data(), static_cast<int>(r.bodies.size()), r.add_state);
                    bi.DestroyBodies(r.bodies.data(), static_cast<int>(r.bodies.size()));
                }
                for (uint32_t mi : r.models) {
                    release_model(mi);
                }
                slot.state = CellState::none;
                continue;
            }
            if (!r.bodies.empty()) {
                bi.AddBodiesFinalize(r.bodies.data(), static_cast<int>(r.bodies.size()), r.add_state,
                                     JPH::EActivation::DontActivate);
                remove_not_solid(r.bodies);
                // Only what stayed: Jolt takes an active body to be in the broad phase, and one that isn't crashes the next step.
                std::erase_if(r.dynamic_bodies, [&bi](const JPH::BodyID id) { return !bi.IsAdded(id); });
                if (!r.dynamic_bodies.empty()) {
                    bi.ActivateBodies(r.dynamic_bodies.data(), static_cast<int>(r.dynamic_bodies.size()));
                }
                bodies += static_cast<uint32_t>(r.bodies.size());
                broad_phase_dirty = true;
            }
            slot.bodies = std::move(r.bodies);
            slot.models = std::move(r.models);
            slot.state = CellState::loaded;
            ++loaded;
        }
        if (i < ready.size()) {
            std::lock_guard lock(queue_mutex);
            done.insert(done.end(), std::make_move_iterator(ready.begin() + static_cast<ptrdiff_t>(i)),
                        std::make_move_iterator(ready.end()));
        }
        {
            std::lock_guard lock(ref_mutex);
            for (const uint32_t ci : rebuild) {
                if (cells[ci].state == CellState::loaded) {
                    unload(ci); // the requests above bring it back
                }
            }
            rebuild.clear();
        }
        // Far cells: unload; far requests: cancel.
        for (uint32_t ci : active) {
            auto& slot = cells[ci];
            if (slot.state == CellState::none || cell_distance(ci, p) <= settings.unload_radius ||
                ms_since(start) > settings.budget_ms) {
                continue;
            }
            if (slot.state == CellState::loaded) {
                unload(ci);
            } else if (slot.state == CellState::requested && !slot.cancelled) {
                slot.cancelled = true;
                std::lock_guard lock(queue_mutex);
                auto it = std::ranges::find(queue, ci, &std::pair<float, uint32_t>::second);
                if (it != queue.end()) {
                    queue.erase(it);
                    --requested;
                    slot.state = CellState::none;
                }
            }
        }
        std::erase_if(active, [&](uint32_t ci) { return cells[ci].state == CellState::none; });
        {
            std::lock_guard lock(solid_mutex);
            JPH::BodyInterface& bi = system.GetBodyInterfaceNoLock();
            for (const Push& p : pushes) {
                const JPH::BodyID id(p.body);
                if (bi.IsAdded(id) && bi.GetMotionType(id) == JPH::EMotionType::Dynamic) {
                    bi.ActivateBody(id);
                    bi.AddImpulse(id, to_jolt(p.linear), JPH::RVec3(to_jolt(p.at)));
                    bi.AddAngularImpulse(id, to_jolt(p.angular));
                }
            }
            pushes.clear();
        }
        // One simulation step per tick: clutter the player pushed moves, the rest sleeps. It also keeps the broad
        // phase tree tight after streaming.
        update_ragdolls_before();
        system.Update(1.0f / 60.0f, 1, temp.get(), jobs.get());
        update_ragdolls_after();
        broad_phase_dirty = false;
    }

    bool ready(const glm::vec3& p) const {
        bool ok = true;
        cells_near(p, settings.ready_radius, [&](uint32_t ci) { ok = ok && cells[ci].state == CellState::loaded; });
        return ok;
    }
};

namespace {

WorldSettings with_defaults(WorldSettings s) {
    if (s.character_layers == 0) {
        s.character_layers = default_character_layers();
    }
    return s;
}

} // namespace

World::World(const std::filesystem::path& cache_root, std::shared_ptr<const osw::World> world, WorldSettings settings)
    : m_settings(with_defaults(settings)), m_impl(std::make_unique<Impl>(cache_root, std::move(world), m_settings)) {}

World::~World() = default;

void World::update(const glm::vec3& focus) {
    std::unique_lock lock(m_impl->query_mutex);
    m_impl->update(focus);
}

bool World::ready(const glm::vec3& p) const { return m_impl->ready(p); }

bool World::busy() const { return m_impl->requested > 0; }

void World::set_ref_disabled(uint32_t form_id, bool disabled) {
    std::lock_guard lock(m_impl->ref_mutex);
    m_impl->ref_disabled[form_id] = disabled;
    for (const uint32_t ci : m_impl->active) {
        const auto& c = m_impl->world->cells[ci];
        const auto& refs = m_impl->world->refs;
        const uint64_t end = std::min<uint64_t>(uint64_t(c.first_ref) + c.ref_count, refs.size());
        for (uint64_t i = c.first_ref; i < end; ++i) {
            if (refs[i].form_id == form_id) {
                m_impl->rebuild.push_back(ci);
                return;
            }
        }
    }
}

WorldStats World::stats() const {
    WorldStats s;
    s.cells_loaded = m_impl->loaded;
    s.cells_loading = m_impl->requested;
    s.bodies = m_impl->bodies;
    std::lock_guard lock(m_impl->model_mutex);
    s.shapes_cached = m_impl->models_alive;
    s.shapes_missing = m_impl->models_missing;
    s.shape_bytes = m_impl->model_bytes;
    return s;
}

uint32_t World::add_ragdoll(RagdollSpec spec) {
    std::lock_guard lock(m_impl->ragdoll_mutex);
    const uint32_t id = m_impl->next_ragdoll++;
    m_impl->ragdolls_to_add.emplace_back(id, std::move(spec));
    return id;
}

void World::remove_ragdoll(uint32_t id) {
    std::lock_guard lock(m_impl->ragdoll_mutex);
    m_impl->ragdolls_to_remove.push_back(id);
}

bool World::ragdoll_state(uint32_t id, std::vector<RagdollBodyState>& out) const {
    std::lock_guard lock(m_impl->ragdoll_mutex);
    auto it = m_impl->ragdoll_states.find(id);
    if (it == m_impl->ragdoll_states.end()) {
        return false;
    }
    out = it->second;
    return true;
}

bool World::add_object(uint32_t form_id, const std::string& mesh, const glm::vec3& position, const glm::quat& rotation,
                       bool fixed) {
    auto file = m_impl->cache_root / path::replace_extension(mesh, ".osp");
    std::error_code missing;
    if (!std::filesystem::exists(file, missing)) {
        return false;
    }
    Impl::Object object;
    object.form_id = form_id;
    object.position = position;
    object.rotation = rotation;
    object.fixed = fixed;
    object.shape = std::async(std::launch::async, [file = std::move(file), layers = m_impl->settings.character_layers] {
        return build_shape(osp::read(read_file(file)), layers);
    });
    std::lock_guard lock(m_impl->ragdoll_mutex);
    m_impl->objects_to_add.push_back(std::move(object));
    return true;
}

void World::remove_object(uint32_t form_id) {
    std::lock_guard lock(m_impl->ragdoll_mutex);
    m_impl->objects_to_remove.push_back(form_id);
}

void World::add_animated(uint32_t form_id, const std::string& file, float scale) {
    const auto full = m_impl->cache_root / file;
    std::error_code missing;
    if (!std::filesystem::exists(full, missing)) {
        return;
    }
    Impl::AnimatedLoad load;
    load.form_id = form_id;
    load.shapes = std::async(std::launch::async, [full, scale, layers = m_impl->settings.character_layers | k_trap_layers] {
        Impl::AnimatedShapes out;
        const osp::Collision all = osp::read(read_file(full));
        for (size_t b = 0; b < all.bodies.size(); ++b) {
            // Build_shape takes a whole file: each body's shapes alone, on their own table of one body.
            osp::Collision one = all;
            std::erase_if(one.shapes, [b](const osp::Shape& s) { return s.body != b; });
            one.bodies = {all.bodies[b]};
            for (osp::Shape& s : one.shapes) {
                s.body = 0;
            }
            if (one.shapes.empty() || all.bodies[b].node == 0) {
                continue;
            }
            const Built built = build_shape(one, layers);
            if (built.shape) {
                Impl::AnimatedBody body;
                body.node = all.bodies[b].node - 1u;
                body.shape = std::abs(scale - 1.0f) > 1e-4f ? JPH::RefConst<JPH::Shape>(new JPH::ScaledShape(built.shape, JPH::Vec3::sReplicate(scale)))
                                                            : built.shape;
                out.push_back(std::move(body));
            }
        }
        return out;
    });
    std::lock_guard lock(m_impl->ragdoll_mutex);
    m_impl->animated_to_add.push_back(std::move(load));
}

void World::remove_animated(uint32_t form_id) {
    std::lock_guard lock(m_impl->ragdoll_mutex);
    m_impl->animated_to_remove.push_back(form_id);
    m_impl->animated_moves.erase(form_id);
}

void World::move_animated(uint32_t form_id, std::vector<NodePose> nodes) {
    std::lock_guard lock(m_impl->ragdoll_mutex);
    m_impl->animated_moves[form_id] = std::move(nodes);
}

void World::impulse_ref(uint32_t form_id, const glm::vec3& impulse) {
    const auto& refs = m_impl->world->refs;
    for (size_t i = 0; i < refs.size(); ++i) {
        if (refs[i].form_id == form_id) {
            std::lock_guard lock(m_impl->ragdoll_mutex);
            m_impl->ref_impulses.emplace_back(static_cast<uint32_t>(i), impulse);
        }
    }
}

void World::steer_ref(uint32_t form_id, const glm::vec3& velocity) {
    const auto& refs = m_impl->world->refs;
    for (size_t i = 0; i < refs.size(); ++i) {
        if (refs[i].form_id == form_id) {
            std::lock_guard lock(m_impl->ragdoll_mutex);
            m_impl->ref_steers.emplace_back(static_cast<uint32_t>(i), velocity);
        }
    }
}

bool World::ref_dynamic(uint32_t form_id) const {
    const auto& refs = m_impl->world->refs;
    for (size_t i = 0; i < refs.size(); ++i) {
        if (refs[i].form_id == form_id) {
            const JPH::BodyID body(uint32_t(m_impl->world->cells.size()) + uint32_t(i));
            auto& bi = m_impl->system.GetBodyInterface();
            return bi.IsAdded(body) && bi.GetMotionType(body) == JPH::EMotionType::Dynamic;
        }
    }
    return false;
}

std::vector<MovedRef> World::take_moved_refs() {
    std::lock_guard lock(m_impl->ragdoll_mutex);
    std::vector<MovedRef> out;
    out.swap(m_impl->moved_since_read);
    return out;
}

void World::push_ragdoll(uint32_t id, const glm::vec3& centre, float strength) {
    std::lock_guard lock(m_impl->ragdoll_mutex);
    m_impl->ragdolls_to_push.push_back({id, centre, strength});
}

bool World::touching_shared(const glm::vec3& a, const glm::vec3& b, float radius) const {
    std::shared_lock lock(m_impl->query_mutex);
    const JPH::RefConst<JPH::Shape> shape = Impl::capsule(glm::vec3(0.0f), b - a, radius, 0);
    JPH::CollideShapeSettings settings;
    JPH::AnyHitCollisionCollector<JPH::CollideShapeCollector> hit;
    m_impl->system.GetNarrowPhaseQueryNoLock().CollideShape(shape, JPH::Vec3::sReplicate(1.0f),
                                                            JPH::RMat44::sTranslation(JPH::RVec3(a.x, a.y, a.z)), settings,
                                                            JPH::RVec3::sZero(), hit, k_no_actors);
    return hit.HadHit();
}

std::optional<RayHit> World::raycast_shared(const glm::vec3& from, const glm::vec3& direction, float max_distance) const {
    std::shared_lock lock(m_impl->query_mutex);
    return raycast(from, direction, max_distance);
}

std::optional<RayHit> World::raycast(const glm::vec3& from, const glm::vec3& direction, float max_distance) const {
    const glm::vec3 d = glm::normalize(direction) * max_distance;
    JPH::RRayCast ray(JPH::RVec3(from.x, from.y, from.z), to_jolt(d));
    JPH::RayCastResult hit;
    const auto& query = m_impl->system.GetNarrowPhaseQueryNoLock();
    if (!query.CastRay(ray, hit, k_no_actors)) {
        return std::nullopt;
    }
    RayHit out;
    out.distance = hit.mFraction * max_distance;
    JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterfaceNoLock(), hit.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Body& body = lock.GetBody();
        const JPH::RVec3 at = ray.GetPointOnRay(hit.mFraction);
        out.normal = to_glm(body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, at));
        const uint64_t ud = body.GetShape()->GetSubShapeUserData(hit.mSubShapeID2);
        out.layer = user_data_layer(ud);
        out.material = user_data_material(ud);
    }
    return out;
}

namespace {

// Passes leaf shapes whose Skyrim layer is in a mask (compounds and wrappers carry no layer).
class LayerShapeFilter final : public JPH::ShapeFilter {
public:
    explicit LayerShapeFilter(uint64_t layers) : m_layers(layers) {}
    bool ShouldCollide(const JPH::Shape* shape, const JPH::SubShapeID&) const override { return passes(shape); }
    bool ShouldCollide(const JPH::Shape*, const JPH::SubShapeID&, const JPH::Shape* shape,
                       const JPH::SubShapeID&) const override {
        return passes(shape);
    }

private:
    bool passes(const JPH::Shape* shape) const {
        const auto type = shape->GetType();
        if (type == JPH::EShapeType::Compound || type == JPH::EShapeType::Decorated) {
            return true;
        }
        return (m_layers >> user_data_layer(shape->GetUserData())) & 1;
    }

    uint64_t m_layers;
};

// The layer filter, further limited to leaf shapes whose Havok shell (convex radius) is `shell`.
class ShellShapeFilter final : public JPH::ShapeFilter {
public:
    ShellShapeFilter(uint64_t layers, uint64_t shell_bits) : m_layers(layers), m_shell(shell_bits) {}
    bool ShouldCollide(const JPH::Shape* shape, const JPH::SubShapeID&) const override { return passes(shape); }
    bool ShouldCollide(const JPH::Shape*, const JPH::SubShapeID&, const JPH::Shape* shape,
                       const JPH::SubShapeID&) const override {
        return passes(shape);
    }

private:
    bool passes(const JPH::Shape* shape) const {
        const auto type = shape->GetType();
        if (type == JPH::EShapeType::Compound || type == JPH::EShapeType::Decorated) {
            return true;
        }
        const uint64_t d = shape->GetUserData();
        return ((m_layers >> user_data_layer(d)) & 1) && ((d >> 40) & 0xFFFF) == m_shell;
    }

    uint64_t m_layers;
    uint64_t m_shell;
};

constexpr float k_max_shell = 0.1f; // the largest convex radius a world shape gets (metres)

} // namespace

std::optional<World::SweepHit> World::sphere_cast(const glm::vec3& from, const glm::vec3& to, float radius,
                                                   uint64_t layers) const {
    // A Havok sphere meets each shape at its outer surface, its convex radius out (a box's lies outside its half
    // extents: measured, camera3, the shack door's 0.027 m box radius stops the camera 1.9 units nearer). Jolt
    // holds the bare shapes, so the sphere is grown by each shell in turn, against the shapes with that shell.
    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
    const auto& query = m_impl->system.GetNarrowPhaseQueryNoLock();
    auto cast_with = [&](float r, const JPH::ShapeFilter& filter, auto& collector) {
        const JPH::SphereShape sphere(r);
        const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
            &sphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(JPH::RVec3(from.x, from.y, from.z)),
            to_jolt(to - from));
        query.CastShape(cast, settings, JPH::RVec3::sZero(), collector, k_no_actors, {}, {}, filter);
    };
    // The shells within reach of the sweep.
    std::vector<uint64_t> shells;
    {
        JPH::AllHitCollisionCollector<JPH::CastShapeCollector> all;
        cast_with(radius + k_max_shell, LayerShapeFilter(layers), all);
        auto& bodies = m_impl->system.GetBodyLockInterfaceNoLock();
        for (const auto& h : all.mHits) {
            JPH::BodyLockRead lock(bodies, h.mBodyID2);
            if (lock.Succeeded()) {
                const uint64_t s = (lock.GetBody().GetShape()->GetSubShapeUserData(h.mSubShapeID2) >> 40) & 0xFFFF;
                if (std::ranges::find(shells, s) == shells.end()) {
                    shells.push_back(s);
                }
            }
        }
    }
    std::optional<SweepHit> best;
    for (const uint64_t s : shells) {
        const float shell = static_cast<float>(s) * 1e-4f;
        JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
        cast_with(radius + shell, ShellShapeFilter(layers, s), collector);
        if (!collector.HadHit() || (best && collector.mHit.mFraction >= best->fraction)) {
            continue;
        }
        const JPH::ShapeCastResult& hit = collector.mHit;
        SweepHit out;
        out.fraction = hit.mFraction;
        out.normal = -to_glm(hit.mPenetrationAxis.NormalizedOr(JPH::Vec3::sAxisZ()));
        out.point = to_glm(JPH::Vec3(hit.mContactPointOn2)) + shell * out.normal; // on the outer surface
        JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterfaceNoLock(), hit.mBodyID2);
        if (lock.Succeeded()) {
            out.layer = user_data_layer(lock.GetBody().GetShape()->GetSubShapeUserData(hit.mSubShapeID2));
        }
        best = out;
    }
    return best;
}

namespace {

World::SweepContact contact_of(const JPH::Body& body, const JPH::SubShapeID& sub, const osw::World& w) {
    World::SweepContact c;
    const uint64_t ud = body.GetShape()->GetSubShapeUserData(sub);
    c.layer = user_data_layer(ud);
    c.material = user_data_material(ud);
    const uint32_t id = body.GetID().GetIndex();
    c.body = id;
    if (id >= w.cells.size() && id - w.cells.size() < w.refs.size()) {
        c.ref = w.refs[id - w.cells.size()].form_id;
    }
    if (body.IsDynamic() && body.GetMotionProperties()->GetInverseMass() > 0.0f) {
        c.mass = 1.0f / body.GetMotionProperties()->GetInverseMass();
    }
    return c;
}

} // namespace

std::vector<World::SweepContact> World::sphere_sweep(const glm::vec3& from, const glm::vec3& to, float radius,
                                                     uint64_t layers) const {
    std::vector<SweepContact> out;
    if (glm::length(to - from) < 1e-5f) {
        return out;
    }
    std::shared_lock world_lock(m_impl->query_mutex);
    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
    const auto& query = m_impl->system.GetNarrowPhaseQueryNoLock();
    auto& bodies = m_impl->system.GetBodyLockInterfaceNoLock();
    auto cast_with = [&](float r, const JPH::ShapeFilter& filter, auto& collector) {
        const JPH::SphereShape sphere(r);
        const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
            &sphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(JPH::RVec3(from.x, from.y, from.z)),
            to_jolt(to - from));
        query.CastShape(cast, settings, JPH::RVec3::sZero(), collector, k_no_actors, {}, {}, filter);
    };
    // Shell by shell, as sphere_cast: each shape is met at its Havok surface.
    std::vector<uint64_t> shells;
    {
        JPH::AllHitCollisionCollector<JPH::CastShapeCollector> all;
        cast_with(radius + k_max_shell, LayerShapeFilter(layers), all);
        for (const auto& h : all.mHits) {
            JPH::BodyLockRead lock(bodies, h.mBodyID2);
            if (lock.Succeeded()) {
                const uint64_t s = (lock.GetBody().GetShape()->GetSubShapeUserData(h.mSubShapeID2) >> 40) & 0xFFFF;
                if (std::ranges::find(shells, s) == shells.end()) {
                    shells.push_back(s);
                }
            }
        }
    }
    for (const uint64_t s : shells) {
        const float shell = static_cast<float>(s) * 1e-4f;
        JPH::AllHitCollisionCollector<JPH::CastShapeCollector> hits;
        cast_with(radius + shell, ShellShapeFilter(layers, s), hits);
        for (const JPH::ShapeCastResult& hit : hits.mHits) {
            JPH::BodyLockRead lock(bodies, hit.mBodyID2);
            if (!lock.Succeeded()) {
                continue;
            }
            SweepContact c = contact_of(lock.GetBody(), hit.mSubShapeID2, *m_impl->world);
            c.fraction = hit.mFraction;
            c.normal = -to_glm(hit.mPenetrationAxis.NormalizedOr(JPH::Vec3::sAxisZ()));
            c.point = to_glm(JPH::Vec3(hit.mContactPointOn2)) + shell * c.normal;
            const auto same = std::ranges::find(out, c.body, &SweepContact::body);
            if (same == out.end()) {
                out.push_back(c);
            } else if (c.fraction < same->fraction) {
                *same = c;
            }
        }
    }
    std::ranges::sort(out, {}, &SweepContact::fraction);
    return out;
}

std::vector<World::SweepContact> World::sphere_overlap(const glm::vec3& centre, float radius, uint64_t layers) const {
    std::vector<SweepContact> out;
    std::shared_lock world_lock(m_impl->query_mutex);
    const JPH::SphereShape sphere(std::max(radius, 0.001f));
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> hits;
    m_impl->system.GetNarrowPhaseQueryNoLock().CollideShape(
        &sphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(JPH::RVec3(centre.x, centre.y, centre.z)), settings,
        JPH::RVec3::sZero(), hits, k_no_actors, {}, {}, LayerShapeFilter(layers));
    auto& bodies = m_impl->system.GetBodyLockInterfaceNoLock();
    for (const auto& hit : hits.mHits) {
        JPH::BodyLockRead lock(bodies, hit.mBodyID2);
        if (lock.Succeeded() && std::ranges::find(out, hit.mBodyID2.GetIndex(), &SweepContact::body) == out.end()) {
            SweepContact c = contact_of(lock.GetBody(), hit.mSubShapeID2, *m_impl->world);
            c.point = to_glm(JPH::Vec3(lock.GetBody().GetPosition()));
            out.push_back(c);
        }
    }
    return out;
}

bool World::cell_loaded(const glm::vec3& p) const {
    std::shared_lock lock(m_impl->query_mutex);
    bool loaded = false;
    m_impl->cells_near(p, 0.0f, [&](uint32_t ci) { loaded = m_impl->cells[ci].state == Impl::CellState::loaded; });
    return loaded;
}

void World::push_body(uint32_t body, const glm::vec3& linear, const glm::vec3& angular, const glm::vec3& at) {
    std::lock_guard lock(m_impl->solid_mutex);
    m_impl->pushes.push_back({body, linear, angular, at});
}

void World::traps_touching(const glm::vec3& a, const glm::vec3& b, float radius, std::vector<TrapTouch>& out) const {
    std::shared_lock lock(m_impl->query_mutex);
    const JPH::RefConst<JPH::Shape> shape = Impl::capsule(glm::vec3(0.0f), b - a, radius, 0);
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> hits;
    LayerShapeFilter traps(k_trap_layers);
    m_impl->system.GetNarrowPhaseQueryNoLock().CollideShape(shape, JPH::Vec3::sReplicate(1.0f),
                                                            JPH::RMat44::sTranslation(JPH::RVec3(a.x, a.y, a.z)), settings,
                                                            JPH::RVec3::sZero(), hits, {}, {}, {}, traps);
    const uint32_t first = uint32_t(m_impl->world->cells.size()), end = first + uint32_t(m_impl->world->refs.size());
    auto& bi = m_impl->system.GetBodyInterfaceNoLock();
    const auto add = [&](uint32_t form, const JPH::CollideShapeResult& hit) {
        if (std::ranges::find(out, form, &TrapTouch::form) == out.end()) {
            const JPH::RVec3 at(hit.mContactPointOn2); // the query's base offset is zero: world space
            const JPH::Vec3 v = bi.GetPointVelocity(hit.mBodyID2, at);
            out.push_back({form, glm::vec3(float(at.GetX()), float(at.GetY()), float(at.GetZ())),
                           glm::vec3(v.GetX(), v.GetY(), v.GetZ())});
        }
    };
    for (const auto& hit : hits.mHits) {
        const uint32_t index = hit.mBodyID2.GetIndex();
        if (index >= first && index < end) {
            add(m_impl->world->refs[index - first].form_id, hit);
        } else {
            std::lock_guard moving(m_impl->ragdoll_mutex);
            if (const auto it = m_impl->animated_by_body.find(index); it != m_impl->animated_by_body.end()) {
                add(it->second, hit);
            }
        }
    }
}

std::optional<glm::vec3> World::ref_position(uint32_t form_id) const {
    const auto& refs = m_impl->world->refs;
    for (size_t i = 0; i < refs.size(); ++i) {
        if (refs[i].form_id != form_id) {
            continue;
        }
        const JPH::BodyID id(static_cast<uint32_t>(m_impl->world->cells.size() + i));
        JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterfaceNoLock(), id);
        if (lock.Succeeded()) {
            return to_glm(JPH::Vec3(lock.GetBody().GetPosition()));
        }
    }
    return std::nullopt;
}

float World::ref_mass(uint32_t form_id) const {
    const auto& refs = m_impl->world->refs;
    for (size_t i = 0; i < refs.size(); ++i) {
        if (refs[i].form_id != form_id) {
            continue;
        }
        const JPH::BodyID id(static_cast<uint32_t>(m_impl->world->cells.size() + i));
        JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterfaceNoLock(), id);
        if (lock.Succeeded() && lock.GetBody().IsDynamic()) {
            const float inverse = lock.GetBody().GetMotionProperties()->GetInverseMass();
            return inverse > 0.0f ? 1.0f / inverse : 0.0f;
        }
    }
    return 0.0f;
}

void World::set_ref_solid(uint32_t form_id, bool solid) {
    // A journey changes a hundred at once, and a world has a quarter of a million references: sorted by id once.
    std::call_once(m_impl->ref_order_once, [this] {
        const auto& refs = m_impl->world->refs;
        m_impl->ref_order.reserve(refs.size());
        for (size_t i = 0; i < refs.size(); ++i) {
            m_impl->ref_order.emplace_back(refs[i].form_id, uint32_t(i));
        }
        std::ranges::sort(m_impl->ref_order);
    });
    const auto& order = m_impl->ref_order;
    auto it = std::ranges::lower_bound(order, std::pair<uint32_t, uint32_t>(form_id, 0));
    std::lock_guard lock(m_impl->solid_mutex);
    for (; it != order.end() && it->first == form_id; ++it) {
        m_impl->solid_changes.emplace_back(it->second, solid);
    }
}

void World::set_actor_bodies(std::span<const ActorBody> actors) {
    std::lock_guard lock(m_impl->solid_mutex);
    if (actors.empty() && m_impl->actor_bodies.empty()) {
        return;
    }
    m_impl->actor_bodies.assign(actors.begin(), actors.begin() + std::min<ptrdiff_t>(actors.size(), k_max_actors));
    m_impl->actor_bodies_changed = true;
}

void World::set_leaves(std::span<const Leaf> leaves) {
    std::lock_guard lock(m_impl->solid_mutex);
    if (leaves.empty() && m_impl->leaves.empty()) {
        return;
    }
    m_impl->leaves.assign(leaves.begin(), leaves.begin() + std::min<ptrdiff_t>(leaves.size(), k_max_leaves));
    m_impl->leaves_changed = true;
}

WorldAccess access(World& world) { return {world.impl().system}; }

} // namespace spud::physics
