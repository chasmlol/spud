#include "anim/ragdoll.hpp"

#include "core/error.hpp"

#include <iterator>
#include <unordered_map>

namespace spud::anim {

namespace {

// A hit's velocity on a body, by the part in its filter word ( in the game).
float part_force_scale(uint32_t part) {
    constexpr float scale[23] = {1.0f,  1.0f, 1.0f,  1.0f, 1.0f,  1.0f, 0.75f, 0.5f, 0.75f, 0.5f,  0.25f, 1.0f,
                                 0.75f, 0.5f, 0.75f, 0.5f, 0.25f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f};
    return part < std::size(scale) ? scale[part] : 0.0f;
}

const osb::Object* follow(const osb::Graph& g, const osb::Value* v) {
    return v && v->kind == osb::Kind::object && v->object < g.objects.size() ? &g.objects[v->object] : nullptr;
}

float real(const osb::Object* o, std::string_view name, float otherwise = 0.0f) {
    const auto* v = o ? o->get(name) : nullptr;
    return v ? v->real : otherwise;
}

Qs qs_of(const std::vector<float>& f) {
    Qs q;
    if (f.size() >= 12) {
        q.t = {f[0], f[1], f[2]};
        q.r = glm::quat(f[7], f[4], f[5], f[6]);
        q.s = {f[8], f[9], f[10]};
    }
    return q;
}

// hkTransform: three columns, then the translation.
void transform_of(const osb::Value* v, glm::mat3& axes, glm::vec3& translation) {
    if (v && v->floats.size() >= 16) {
        const auto& f = v->floats;
        axes = glm::mat3(glm::vec3(f[0], f[1], f[2]), glm::vec3(f[4], f[5], f[6]), glm::vec3(f[8], f[9], f[10]));
        translation = {f[12], f[13], f[14]};
    }
}

std::vector<RagdollMapping> mappings(const osb::Graph& g, const osb::Object& data) {
    std::vector<RagdollMapping> out;
    if (const auto* list = data.get("simpleMappings")) {
        for (const auto& v : list->list) {
            const osb::Object* m = follow(g, &v);
            if (!m) {
                continue;
            }
            RagdollMapping r;
            r.bone_a = static_cast<int32_t>(m->get("boneA")->integer);
            r.bone_b = static_cast<int32_t>(m->get("boneB")->integer);
            r.a_from_b = qs_of(m->get("aFromBTransform")->floats);
            out.push_back(r);
        }
    }
    return out;
}

} // namespace

std::shared_ptr<const Ragdoll> read_ragdoll(const osb::Graph& g) {
    const osb::Object* instance = nullptr;
    std::vector<const osb::Object*> mappers;
    for (const auto& o : g.objects) {
        if (o.cls == "hkaRagdollInstance") {
            instance = &o;
        } else if (o.cls == "hkaSkeletonMapper") {
            mappers.push_back(&o);
        }
    }
    if (!instance) {
        return nullptr;
    }
    auto out = std::make_shared<Ragdoll>();
    std::unordered_map<uint32_t, int32_t> index; // rigid body object to its place
    const auto* bodies = instance->get("rigidBodies");
    if (!bodies) {
        throw ParseError("ragdoll without bodies");
    }
    for (const auto& v : bodies->list) {
        const osb::Object* body = follow(g, &v);
        if (!body) {
            continue;
        }
        RagdollBody b;
        if (const auto* n = body->get("name")) {
            b.name = n->text;
        }
        const osb::Object* collidable = follow(g, body->get("collidable"));
        const osb::Object* shape = collidable ? follow(g, collidable->get("shape")) : nullptr;
        if (shape) {
            b.radius = real(shape, "radius");
            if (const auto* a = shape->get("vertexA"); a && a->floats.size() >= 3) {
                b.a = {a->floats[0], a->floats[1], a->floats[2]};
            }
            if (const auto* e = shape->get("vertexB"); e && e->floats.size() >= 3) {
                b.b = {e->floats[0], e->floats[1], e->floats[2]};
            }
        }
        if (const osb::Object* handle = collidable ? follow(g, collidable->get("broadPhaseHandle")) : nullptr) {
            if (const auto* f = handle->get("collisionFilterInfo")) {
                b.collision_filter = static_cast<uint32_t>(f->integer);
                b.force_scale = part_force_scale(b.collision_filter >> 8 & 0x1F);
            }
        }
        const osb::Object* material = follow(g, body->get("material"));
        b.friction = real(material, "friction");
        b.restitution = real(material, "restitution");
        const osb::Object* motion = follow(g, body->get("motion"));
        if (motion) {
            if (const auto* i = motion->get("inertiaAndMassInv"); i && i->floats.size() >= 4) {
                b.inverse_inertia = {i->floats[0], i->floats[1], i->floats[2]};
                b.mass = i->floats[3] > 0.0f ? 1.0f / i->floats[3] : 0.0f;
            }
            b.gravity_factor = real(motion, "gravityFactor", 1.0f);
            if (const osb::Object* state = follow(g, motion->get("motionState"))) {
                glm::mat3 axes(1.0f);
                glm::vec3 position(0.0f);
                transform_of(state->get("transform"), axes, position);
                b.rest.t = position;
                b.rest.r = glm::quat_cast(axes);
                b.linear_damping = real(state, "linearDamping");
                b.angular_damping = real(state, "angularDamping");
                b.max_linear_velocity = real(state, "maxLinearVelocity");
                b.max_angular_velocity = real(state, "maxAngularVelocity");
            }
        }
        index.emplace(v.object, static_cast<int32_t>(out->bodies.size()));
        out->bodies.push_back(std::move(b));
    }
    if (const auto* constraints = instance->get("constraints")) {
        for (const auto& v : constraints->list) {
            const osb::Object* c = follow(g, &v);
            const auto* entities = c ? c->get("entities") : nullptr;
            const osb::Object* data = c ? follow(g, c->get("data")) : nullptr;
            const osb::Object* atoms = data ? follow(g, data->get("atoms")) : nullptr;
            if (!atoms || !entities || entities->list.size() < 2) {
                continue;
            }
            auto a = index.find(entities->list[0].object), b = index.find(entities->list[1].object);
            if (a == index.end() || b == index.end()) {
                continue;
            }
            RagdollConstraint r;
            r.body_a = a->second;
            r.body_b = b->second;
            r.hinge = data->cls == "hkpLimitedHingeConstraintData";
            if (const osb::Object* t = follow(g, atoms->get("transforms"))) {
                transform_of(t->get("transformA"), r.axes_a, r.pivot_a);
                transform_of(t->get("transformB"), r.axes_b, r.pivot_b);
            }
            r.friction = real(follow(g, atoms->get("angFriction")), "maxFrictionTorque");
            if (r.hinge) {
                const osb::Object* limit = follow(g, atoms->get("angLimit"));
                r.hinge_min = real(limit, "minAngle");
                r.hinge_max = real(limit, "maxAngle");
            } else {
                const osb::Object* twist = follow(g, atoms->get("twistLimit"));
                r.twist_min = real(twist, "minAngle");
                r.twist_max = real(twist, "maxAngle");
                r.cone_max = real(follow(g, atoms->get("coneLimit")), "maxAngle");
                const osb::Object* planes = follow(g, atoms->get("planesLimit"));
                r.plane_min = real(planes, "minAngle");
                r.plane_max = real(planes, "maxAngle");
            }
            out->constraints.push_back(r);
        }
    }
    if (const auto* map = instance->get("boneToRigidBodyMap")) {
        out->bone_to_body = map->ints;
    }
    const osb::Object* skeleton = follow(g, instance->get("skeleton"));
    if (skeleton) {
        if (const auto* p = skeleton->get("parentIndices")) {
            out->parents.assign(p->ints.begin(), p->ints.end());
        }
    }
    for (const osb::Object* m : mappers) {
        const osb::Object* data = follow(g, m->get("mapping"));
        if (!data) {
            continue;
        }
        // Mapper A -> B: from the ragdoll is the one whose skeleton A is the instance's.
        const bool from_ragdoll = skeleton && data->get("skeletonA")->object == instance->get("skeleton")->object;
        (from_ragdoll ? out->to_animation : out->to_ragdoll) = mappings(g, *data);
    }
    return out;
}

void Ragdoll::bodies_from_pose(std::span<const Qs> model_pose, std::vector<Qs>& out) const {
    out.resize(bodies.size());
    for (size_t i = 0; i < bodies.size(); ++i) {
        out[i] = bodies[i].rest;
    }
    for (const RagdollMapping& m : to_ragdoll) {
        if (m.bone_a < 0 || size_t(m.bone_a) >= model_pose.size() || m.bone_b < 0 || size_t(m.bone_b) >= bone_to_body.size()) {
            continue;
        }
        const int32_t body = bone_to_body[size_t(m.bone_b)];
        if (body >= 0 && size_t(body) < out.size()) {
            out[size_t(body)] = model_pose[size_t(m.bone_a)] * m.a_from_b;
        }
    }
}

void Ragdoll::pose_from_bodies(std::span<const Qs> bodies_model, std::span<const int16_t> bone_parents,
                               std::span<Qs> local_pose) const {
    const size_t n = bone_parents.size();
    std::vector<Qs> model(n);
    std::vector<uint8_t> mapped(n, 0);
    for (const RagdollMapping& m : to_animation) {
        if (m.bone_b < 0 || size_t(m.bone_b) >= n || m.bone_a < 0 || size_t(m.bone_a) >= bone_to_body.size()) {
            continue;
        }
        const int32_t body = bone_to_body[size_t(m.bone_a)];
        if (body >= 0 && size_t(body) < bodies_model.size()) {
            model[size_t(m.bone_b)] = bodies_model[size_t(body)] * m.a_from_b;
            mapped[size_t(m.bone_b)] = 1;
        }
    }
    for (size_t i = 0; i < n; ++i) { // parents come first
        const int16_t p = bone_parents[i];
        if (!mapped[i]) {
            model[i] = p >= 0 ? model[size_t(p)] * local_pose[i] : local_pose[i];
        } else if (p >= 0) {
            const Qs& up = model[size_t(p)];
            const glm::quat back = glm::conjugate(up.r);
            local_pose[i].t = (back * (model[i].t - up.t)) / up.s;
            local_pose[i].r = back * model[i].r;
        } else {
            local_pose[i] = model[i];
        }
    }
}

} // namespace spud::anim
