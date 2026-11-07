// Load doors as seamless portals between spaces. A load door opens (the
// player activates it, or the free camera comes near) and shows the space behind it through its doorway; walking
// through the doorway carries the camera, and the player, into that space. Nothing loads at the crossing: the far
// space is read on a worker thread when its door comes within reach and streamed from the camera carried through.

#include "engine/app_internal.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/units.hpp"
#include "engine/weather.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_set>

namespace spud::engine {

namespace {

constexpr float k_preload_m = 40.0f;   // a door this close has its far space read and streamed
constexpr float k_auto_open_m = 3.0f;  // the free camera opens doors this close
constexpr float k_close_m = 8.0f;      // open doors close once the camera is this far from both sides
constexpr float k_portal_m = 80.0f;    // the view through a door is drawn while it is this close
// Doors in one space this close and facing apart are the two faces of one gateway: a merged city's gate.
// A merged city's gate pair shares one arch: its two doors stand within this of each other, with open ground between. Pairs
// further apart (Solitude's, 4.8 m; Riften's) have wall between them, so the view is carried through as any door's.
constexpr float k_gate_m = 2.5f;
// fActivatePickLength:Interface (180 in the image): how far the player reaches to activate.
constexpr float k_activate_m = 180.0f * units::k_skyrim_unit_m;
constexpr float k_room_door_m = 1.0f; // how far from a door's middle the camera may point and still have it
// A far space's instance table: a slot per reference and these, so an interior's fits in memory already allocated.
constexpr uint32_t k_spare_instances = 512; // spawned models and actors, whose moves take fresh slots
constexpr size_t k_kept_interiors = 12;   // beyond this many, the interior needed longest ago is let go
constexpr uint64_t k_retire_frames = 16;  // a let-go interior's memory is safe to free this many frames later
constexpr float k_physics_m = 10.0f;      // a walker this close to a door has the far side's collision streamed
constexpr uint64_t k_idle_stream_frames = 256; // a loaded space behind a shut door checks its streaming this seldom
// How far an open doorway's passage runs each way, past whatever wall stands across it: the deepest door frames, and the
// player's half depth beyond the plane the camera crosses.
constexpr float k_passage_m = 1.0f;
constexpr float k_travel_clear_m = 0.3f; // how far clear of the door it came out of the camera settles a crossing
// How far from an open doorway the grade starts going over to the other side's. A design value: Skyrim has no seamless
// door to take one from.
constexpr float k_grade_m = 1.5f;
// The main view's effects this near the camera go through a doorway with the player: what it holds or casts.
constexpr float k_carried_particles_m = 1.5f;
constexpr float k_camera_doorway_m = 6.0f; // the third-person camera reaches through a doorway the body is this near
constexpr float k_floor_sample_m = 0.5f; // the floor of a doorway is measured this far in front of it
constexpr float k_floor_reach_m = 1.0f;  // ... and this far above and below its bottom

// The near plane of `v` becomes the plane through `point` with `normal` (the visible side): an oblique projection
// (reverse-Z, infinite far). Its depth stays >= 0 everywhere in the frustum: the plane's term is scaled so that the
// steepest frustum ray keeps depth 0 at infinity. False when nothing beyond the plane is in view.
bool set_near_plane(render::View& v, const glm::vec3& point, const glm::vec3& normal) {
    const glm::vec3 n = glm::mat3(v.rotation) * normal;
    const float d = -glm::dot(normal, point - v.position);
    if (d >= 0.0f) {
        return false; // the camera is already beyond the plane
    }
    const float ty = std::tan(v.fov_y * 0.5f), tx = ty * v.aspect;
    float most = 0.0f;
    for (float sx : {-1.0f, 1.0f}) {
        for (float sy : {-1.0f, 1.0f}) {
            most = std::max(most, glm::dot(n, glm::vec3(sx * tx, sy * ty, -1.0f)));
        }
    }
    if (most <= 0.0f) {
        return false;
    }
    const float k = 1.0f / most;
    glm::mat4& p = v.projection;
    p[0][2] = -k * n.x;
    p[1][2] = -k * n.y;
    p[2][2] = -1.0f - k * n.z;
    p[3][2] = -k * d;
    v.view_proj = p * v.rotation;
    v.frustum = render::Frustum::from_view_proj(v.view_proj);
    return true;
}

// Its side planes close in on the doorway on screen, and the margin the far frame's image passes draw past it
// (Renderer's far scissor): what falls outside was never going to show, and a street costs as much as its vertices.
void narrow_to_doorway(render::View& v, const Doorway& d) {
    glm::vec2 lo(1e30f), hi(-1e30f);
    for (int k = 0; k < 4; ++k) {
        const glm::vec4 c = v.view_proj * glm::vec4(d.corner(k) - v.position, 1.0f);
        if (c.w <= v.near_plane) {
            return; // about to be walked through: the whole view
        }
        lo = glm::min(lo, glm::vec2(c) / c.w);
        hi = glm::max(hi, glm::vec2(c) / c.w);
    }
    constexpr float k_margin_ndc = 0.15f; // over 128 of 2560 pixels' worth, each side
    lo -= k_margin_ndc;
    hi += k_margin_ndc;
    if (lo.x <= -1.0f && lo.y <= -1.0f && hi.x >= 1.0f && hi.y >= 1.0f) {
        return;
    }
    const glm::mat4& m = v.view_proj;
    auto row = [&](int i) { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
    const glm::vec4 x = row(0), y = row(1), w = row(3);
    const glm::vec4 sides[4] = {x - std::max(lo.x, -1.0f) * w, std::min(hi.x, 1.0f) * w - x, y - std::max(lo.y, -1.0f) * w,
                                std::min(hi.y, 1.0f) * w - y};
    for (int i = 0; i < 4; ++i) {
        v.frustum.planes[i] = sides[i] / glm::length(glm::vec3(sides[i]));
    }
}

bool quad_in_view(const render::View& v, const Doorway& d) {
    for (int i = 0; i < 5; ++i) {
        const glm::vec4& plane = v.frustum.planes[i];
        // In front of the eye, not of the near plane: walking up to the door takes it nearer than that.
        const float w = i == 4 ? 0.0f : plane.w;
        bool any = false;
        for (int k = 0; k < 4 && !any; ++k) {
            any = glm::dot(glm::vec3(plane), d.corner(k) - v.position) + w >= 0.0f;
        }
        if (!any) {
            return false;
        }
    }
    return true;
}

} // namespace

void App::rebuild_doors() {
    if (m_door_anims.empty()) {
        load_door_animations();
    }
    m_doors.clear();
    m_portal_door = -1;
    if (!m_space) {
        return;
    }
    const auto& doors = m_space->world().doors;
    for (uint32_t i = 0; i < doors.size(); ++i) {
        const osw::Door& d = doors[i];
        bool disabled = (d.flags & osw::door_initially_disabled) != 0;
        if (m_game) {
            if (const auto it = m_game->ref_overrides().find(d.form_id); it != m_game->ref_overrides().end()) {
                disabled = it->second;
            }
        }
        if (disabled && !m_space_settings.show_disabled) {
            continue; // quest alternatives often stand exactly where the enabled door does
        }
        DoorPortal p;
        p.index = i;
        p.same_space = (d.flags & osw::door_same_space) != 0;
        p.automatic = (d.flags & osw::door_automatic) != 0;
        // The near doorway faces where its partner's teleport arrives; without one, where this door's own arrival
        // marker would be if both doors shared their frame (a merged city's gate).
        const glm::vec3 marker = (d.flags & osw::door_has_marker)
                                     ? glm::vec3(d.marker_position[0], d.marker_position[1], d.marker_position[2])
                                     : glm::vec3(d.position[0], d.position[1], d.position[2]);
        const auto near = make_doorway(d.position, d.rotation, d.scale, d.bounds_min, d.bounds_max, marker);
        const glm::vec3 arrival(d.arrival_position[0], d.arrival_position[1], d.arrival_position[2]);
        const auto far = make_doorway(d.dest_position, d.dest_rotation, d.dest_scale, d.dest_bounds_min,
                                      d.dest_bounds_max, arrival);
        if (!near) {
            continue;
        }
        p.near = *near;
        if (far && (d.flags & osw::door_has_marker)) {
            p.far = *far;
            p.to_far = portal_transform(p.near, p.far);
            p.seamless = true;
            p.gate = p.same_space && glm::dot(p.near.normal, p.far.normal) < -0.7f &&
                     glm::length(p.near.centre - p.far.centre) < k_gate_m;
        } else if (far && !p.same_space) {
            // Nothing says which way this door faces: it faces whoever comes to it.
            p.far = *far;
            p.to_far = portal_transform(p.near, p.far);
            p.seamless = true;
            p.faces_camera = true;
        } else {
            p.gate = p.same_space;
        }
        if (!p.automatic && !m_door_anims.empty() && !m_door_anims.contains(d.base_form_id)) {
            p.travels = true;
            p.seamless = p.gate = false;
        }
        m_doors.push_back(p);
    }
    std::erase_if(m_room_doors_found, [](const RoomDoors& r) { return r.world.expired(); });
    const std::shared_ptr<const osw::World> world = m_space->world_ptr();
    const auto found = std::find_if(m_room_doors_found.begin(), m_room_doors_found.end(), [&](const RoomDoors& r) {
        return r.world.lock() == world && r.show_disabled == m_space_settings.show_disabled;
    });
    if (found != m_room_doors_found.end()) {
        m_room_doors = found->refs;
    } else {
        m_room_doors.clear();
        std::unordered_set<uint32_t> load_doors;
        for (const osw::Door& d : doors) {
            load_doors.insert(d.form_id);
        }
        const auto& refs = m_space->world().refs;
        for (uint32_t i = 0; i < refs.size(); ++i) {
            const auto anim = m_door_anims.find(refs[i].base_form_id);
            if (anim != m_door_anims.end() && !anim->second.hung && !load_doors.contains(refs[i].form_id) &&
                (!(refs[i].flags & osw::ref_initially_disabled) || m_space_settings.show_disabled)) {
                m_room_doors.push_back(i);
            }
        }
        m_room_doors_found.push_back(RoomDoors{world, m_space_settings.show_disabled, m_room_doors});
    }
    log::info("{}: {} load doors ({} seamless)", m_space->edid(), m_doors.size(),
              std::count_if(m_doors.begin(), m_doors.end(), [](const DoorPortal& p) { return p.seamless; }));
}

App::OtherSpace* App::other_space(const std::string& name, bool load) {
    for (auto& o : m_others) {
        if (o->name == name) {
            o->used = m_door_frame;
            return o.get();
        }
    }
    if (!load) {
        return nullptr;
    }
    auto o = std::make_unique<OtherSpace>();
    o->name = name;
    o->used = m_door_frame;
    // A worker reads the file and builds the space, GPU tables included: none of it touches the frame.
    o->loading = std::async(std::launch::async, [this, name] {
        auto far_space = std::make_unique<Space>(*m_renderer, Space::read_world(Space::find_file(m_opt.cache, name)),
                                                       k_spare_instances);
        if (m_atmosphere) {
            far_space->set_sky_static_bases(m_atmosphere->sky_statics);
            far_space->set_texture_swaps(texture_swaps());
        }
        if (m_actor_data) {
            far_space->attach_actors(*m_actor_data);
        }
        return far_space;
    });
    m_others.push_back(std::move(o));
    return m_others.back().get();
}

// A Show Sky interior's weather: a pick from its XCCM region, made once a visit so the door shows what's walked into. The
// game makes it on arrival, and another from outside on leaving.
uint32_t App::interior_sky_weather(const Space& space) {
    const osw::World& w = space.world();
    if (!w.lighting.sky_weather || !m_sky || !m_game) {
        return w.lighting.sky_weather;
    }
    const auto [it, fresh] = m_interior_sky.try_emplace(w.form_id, w.lighting.sky_weather);
    if (fresh) {
        if (const forms::FormEntry* cell = m_game->db().find(w.form_id)) {
            if (const auto region = m_game->db().field(*cell, forms::sig("XCCM"))) {
                if (const uint32_t pick = m_sky->pick_for_region(region->as<uint32_t>())) {
                    it->second = pick;
                }
            }
        }
    }
    return it->second;
}

void App::evaluate_environment(Space& space, float dt) {
    auto& env = space.environment();
    const auto& w = space.world();
    const bool interior = space.interior();
    const bool show_sky = interior && (w.flags & osw::world_show_sky) != 0;
    const bool sky_lighting = show_sky && (w.flags & osw::world_use_sky_lighting) != 0;
    // Show Sky interiors keep the climate last in force (App::m_sky_world), under the sky Sky::set_interior gave them.
    const bool lit = m_sky && (!interior ? m_sky->evaluate_outdoors(env)
                                         : show_sky && m_sky->evaluate_interior(env, !sky_lighting ? 2 : 3,
                                                                                interior_sky_weather(space)));
    if (!lit) {
        env.sky.exterior = false;
        if (m_sky) {
            m_sky->evaluate_indoors(env);
        }
    }
    if (!interior) {
        if (!lit) {
            const float az = glm::radians(m_sun_azimuth), el = glm::radians(m_sun_elevation);
            env.sun_dir = glm::normalize(glm::vec3(std::sin(az) * std::cos(el), std::cos(az) * std::cos(el), std::sin(el)));
        }
    } else if (!sky_lighting) {
        const glm::vec3 zenith = env.sky_zenith, horizon = env.sky_horizon, ground = env.sky_ground;
        const Space::RoomBlend blend = space.room_blend(dt);
        evaluate_interior(*blend.from, blend.sunlight_scale, env, blend.to, blend.weight);
        if (m_sky) {
            m_sky->set_cell_image_space(env, w.lighting.image_space, blend.from_image_space, blend.to_image_space,
                                        blend.weight);
        }
        if (show_sky) {
            env.sky_zenith = zenith, env.sky_horizon = horizon, env.sky_ground = ground;
        }
        // Mode 1: the camera's far plane is the fog clip distance.
        space.set_view_distance(!show_sky && w.lighting.fog_clip > 0.0f
                                    ? units::to_metres(std::min(w.lighting.fog_clip, 353840.0f))
                                    : 0.0f);
    }
    if (m_sky) {
        space.set_sky_model(lit ? m_sky->sky_model() : std::string(),
                            m_camera.position + env.sky.model_offset * units::k_skyrim_unit_m, m_sky->sky_model_fade());
        if (lit) {
            space.show_sky_statics(m_sky->sky_statics(false), m_sky->sky_statics(true), env.sky.sky_statics);
        }
    }
}

bool App::set_door_open(DoorPortal& door, bool open, bool activated) {
    if (!m_space) {
        return false;
    }
    const osw::Door& d = m_space->world().doors[door.index];
    DoorSwing* swing = find_swing(d.form_id);
    const auto track_seconds = [](const std::vector<oda::Pose>& t) {
        return float(t.size() - 1) / oda::k_sample_rate;
    };
    if (door.open == open) {
        door.activated = door.activated || (open && activated);
        if (open && swing && swing->closing) {
            // Back the way it came, from where it has got to.
            const oda::Part& p = swing->anim->parts[0];
            swing->time = track_seconds(p.open) * (1.0f - swing->time / std::max(track_seconds(p.close), 1e-3f));
            swing->closing = false;
            play_door_sound(swing->anim, true, swing->position);
        }
        return true;
    }
    // A door that moves stays open until it has shut.
    if (!open && swing) {
        if (!swing->closing) {
            const oda::Part& p = swing->anim->parts[0];
            swing->time = track_seconds(p.close) * (1.0f - std::min(swing->time / std::max(track_seconds(p.open), 1e-3f), 1.0f));
            swing->closing = true;
            door.activated = false;
            play_door_sound(swing->anim, false, swing->position);
        }
        return true;
    }
    OtherSpace* other = nullptr;
    if (!door.same_space) {
        if (!door.seamless) {
            return false;
        }
        other = other_space(d.destination_space, open);
        if (open && (!other || !other->space)) {
            return false; // not streamed in yet: the door stays shut
        }
    }
    door.open = open;
    door.activated = open && activated;
    if (m_game) {
        m_game->script_event(d.form_id, open ? "OnOpen" : "OnClose", {{activated ? game::k_player_ref : 0u}});
    }
    // Nothing of an open door collides. A door with no animation of its own isn't drawn.
    m_space->set_ref_hidden(d.form_id, open);
    if (m_physics) {
        m_physics->set_ref_solid(d.form_id, !open);
    }
    if (door.same_space) {
        m_space->set_ref_hidden(d.destination, open);
        if (m_physics) {
            m_physics->set_ref_solid(d.destination, !open);
        }
    } else if (other && other->space) {
        other->space->set_ref_hidden(d.destination, open);
        if (other->physics) {
            other->physics->set_ref_solid(d.destination, !open);
        }
    }
    if (open && !swing) {
        start_swing(d, door);
    }
    log::info("door {:08X} ({} -> {}) {}", d.form_id, m_space->edid(), d.destination_space,
              open ? "opened" : "closed");
    return true;
}

App::DoorSwing* App::find_swing(uint32_t door) {
    auto it = std::find_if(m_swings.begin(), m_swings.end(),
                           [&](const DoorSwing& s) { return s.door == door || s.partner == door; });
    return it == m_swings.end() ? nullptr : &*it;
}

Space* App::space_named(const std::string& name) {
    if (m_space && path::to_lower(m_space->edid()) == name) {
        return m_space.get();
    }
    for (auto& o : m_others) {
        if (o->space && o->name == name) {
            return o->space.get();
        }
    }
    return nullptr;
}

namespace {

glm::mat3 ref_rotation(const float rotation[4], float scale) {
    return glm::mat3_cast(glm::quat(rotation[3], rotation[0], rotation[1], rotation[2])) * scale;
}

// Half a turn about the upright of a doorway.
const glm::mat3 k_half_turn(-1.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f);

} // namespace

void App::start_swing(const osw::Door& d, const DoorPortal& door) {
    auto it = m_door_anims.find(d.base_form_id);
    if (it == m_door_anims.end() || it->second.parts.empty() || !door.seamless) {
        return;
    }
    DoorSwing s;
    s.door = d.form_id;
    s.partner = d.destination;
    s.space = path::to_lower(m_space->edid());
    s.partner_space = door.same_space ? s.space : d.destination_space;
    s.anim = &it->second;
    s.rotation = ref_rotation(d.rotation, d.scale);
    s.position = glm::vec3(d.position[0], d.position[1], d.position[2]);
    s.partner_rotation = ref_rotation(d.dest_rotation, d.dest_scale);
    s.partner_position = glm::vec3(d.dest_position[0], d.dest_position[1], d.dest_position[2]);
    s.gate = door.gate;
    if (const Space::TurnedDoor* turned = m_space->turned_door(d.form_id)) {
        s.rotation = k_half_turn * s.rotation;
        s.position = turned->axis + k_half_turn * (s.position - turned->axis);
    }
    s.to_partner = door.to_far;
    s.near = door.near;
    s.far = door.far;
    if (Space* far = space_named(s.partner_space)) {
        for (const osw::Door& o : far->world().doors) {
            if (o.form_id == d.destination) {
                if (auto p = m_door_anims.find(o.base_form_id); p != m_door_anims.end()) {
                    s.partner_anim = &p->second;
                }
                break;
            }
        }
    }
    play_door_sound(s.anim, true, s.position);
    m_swings.push_back(std::move(s));
}

// The game stands the same door on both sides of a doorway, front out, each opening away from whoever faces it: seen from
// inside, the hinge is on the wrong side for the door seen from outside. There is one door here, hung from the outside
// (between two spaces of a kind, from the lower FormID's): the other side shows the back of that door, and pulls it open.
void App::turn_doors(Space& space) {
    std::vector<Space::TurnedDoor> turned;
    for (const osw::Door& d : space.world().doors) {
        const bool to_interior = (d.flags & osw::door_to_interior) != 0;
        const bool inside = space.interior() == to_interior ? d.form_id > d.destination : space.interior();
        auto it = m_door_anims.find(d.base_form_id);
        if (!inside || !(d.flags & osw::door_has_marker) || (d.flags & osw::door_same_space) ||
            it == m_door_anims.end() || !it->second.has_back) {
            continue;
        }
        const oda::Door& anim = it->second;
        const glm::vec3 marker(d.marker_position[0], d.marker_position[1], d.marker_position[2]);
        const auto way = make_doorway(d.position, d.rotation, d.scale, d.bounds_min, d.bounds_max, marker);
        if (!way) {
            continue;
        }
        Space::TurnedDoor t;
        t.form_id = d.form_id;
        t.axis = way->centre;
        const glm::mat3 rotation = ref_rotation(d.rotation, d.scale);
        const glm::vec3 position(d.position[0], d.position[1], d.position[2]);
        const glm::mat3 hung = k_half_turn * rotation;
        const glm::vec3 at = t.axis + k_half_turn * (position - t.axis);
        for (const oda::Part& part : anim.parts) {
            const oda::Pose& p = part.open.front();
            const glm::quat q(p.rotation[3], p.rotation[0], p.rotation[1], p.rotation[2]);
            const glm::vec3 moved(p.translation[0], p.translation[1], p.translation[2]);
            for (uint32_t submesh : part.submeshes) {
                t.shapes.push_back({anim.mesh, submesh, hung * glm::mat3_cast(q), at + hung * moved});
            }
        }
        for (const auto* list : {&anim.still, &anim.backing}) {
            for (uint32_t submesh : *list) {
                t.shapes.push_back({anim.mesh, submesh, rotation, position});
            }
        }
        space.set_ref_hidden(d.form_id, true);
        turned.push_back(std::move(t));
    }
    space.set_turned_doors(std::move(turned));
}

void App::play_door_sound(const oda::Door* anim, bool open, const glm::vec3& position) {
    const uint32_t form = open ? anim->open_sound : anim->close_sound;
    if (!m_audio || !form) {
        return;
    }
    if (auto descriptor = m_audio->find_descriptor(std::format("{:08X}", form))) {
        m_audio->play(*descriptor, position);
    }
}

void App::set_door_shut(uint32_t form_id, const std::string& space) {
    if (Space* s = space_named(space)) {
        s->set_ref_hidden(form_id, s->turned_door(form_id) != nullptr);
    }
    if (m_space && space == path::to_lower(m_space->edid())) {
        if (m_physics) {
            m_physics->set_ref_solid(form_id, true);
        }
        return;
    }
    for (auto& o : m_others) {
        if (o->name == space && o->physics) {
            o->physics->set_ref_solid(form_id, true);
        }
    }
}

namespace {

// How far light from a doorway reaches into the other space (metres): the doorway fills 2% of a hemisphere there.
constexpr float k_door_light_m = 7.0f;

// What a space's lights give at a doorway.
glm::vec3 door_lamps(Space& from, const Doorway& at) {
    glm::vec3 colour(0.0f);
    for (const render::PointLight& p : from.point_lights()) {
        const glm::vec3 d = p.position - at.centre;
        const float dist = glm::length(d);
        if ((p.flags & render::point_light_on) && dist < p.radius && dist > 0.0f) {
            const float a = dist / p.radius;
            colour += p.color * (1.0f - a * a) * std::max(glm::dot(d / dist, at.normal), 0.0f);
        }
    }
    return colour;
}

// An open doorway as the space of `to` sees it: the other side's light comes in through its rectangle.
render::DoorLight door_light(const Doorway& to, float open) {
    render::DoorLight l;
    l.position = to.centre;
    l.radius = k_door_light_m;
    l.normal = to.normal;
    l.openness = open;
    l.right = to.right;
    l.half_width = to.half_width;
    l.half_height = to.half_height;
    return l;
}

} // namespace

void App::update_swings(float time) {
    const float dt = std::max(time - m_swing_time, 0.0f);
    m_swing_time = time;
    const auto last = [](const std::vector<oda::Pose>& t) { return float(t.size() - 1) / oda::k_sample_rate; };
    std::unordered_map<Space*, std::vector<Space::DoorShape>> shapes;
    std::unordered_map<Space*, std::vector<render::DoorLight>> lights;
    std::unordered_map<physics::World*, std::vector<physics::World::Leaf>> solid;
    for (size_t i = 0; i < m_swings.size();) {
        DoorSwing& s = m_swings[i];
        const oda::Part& first = s.anim->parts[0];
        s.time = std::min(s.time + dt, last(s.closing ? first.close : first.open));
        if (s.closing && s.time >= last(first.close)) {
            for (auto& p : m_doors) {
                const uint32_t form = m_space->world().doors[p.index].form_id;
                if (form == s.door || form == s.partner) {
                    p.open = false;
                }
            }
            set_door_shut(s.door, s.space);
            if (s.partner) {
                set_door_shut(s.partner, s.partner_space);
            }
            log::info("door {:08X} shut", s.door);
            m_swings.erase(m_swings.begin() + std::ptrdiff_t(i));
            continue;
        }
        ++i;
        Space* near = space_named(s.space);
        Space* far = s.partner ? space_named(s.partner_space) : nullptr;
        // Past the doorway's plane the leaf is in the far frame, which a door within one space carries through too (Solitude's
        // gate): only a gate on real ground shows it where it is.
        const bool carried = far && (far != near || !s.gate);
        const auto rotate = [&](const glm::mat3& m) {
            return glm::mat3(s.to_partner.vector(m[0]), s.to_partner.vector(m[1]), s.to_partner.vector(m[2]));
        };
        // Between two spaces each side lights the other through the opening, as far as the door has opened.
        uint32_t leaf = 0, near_leaf = 0;
        if (near && far && near != far) {
            const float done = s.time / std::max(last(s.closing ? first.close : first.open), 1e-3f);
            const float open = std::sin((s.closing ? 1.0f - done : done) * 1.5707964f);
            // Each way: which side is the brighter is the shading's to say, surface by surface (through_doors).
            render::DoorLight into_far = door_light(s.far, open);
            render::DoorLight into_near = door_light(s.near, open);
            const PortalTransform back = s.to_partner.inverse();
            const glm::vec3 near_lamps = door_lamps(*near, s.near), far_lamps = door_lamps(*far, s.far);
            for (int r = 0; r < 3; ++r) {
                const glm::vec3 a = near->environment().dir_ambient[r], b = far->environment().dir_ambient[r];
                into_far.ambient[r] = glm::vec4(s.to_partner.vector(a), near->environment().dir_ambient[r].w + near_lamps[r]);
                into_near.ambient[r] = glm::vec4(back.vector(b), far->environment().dir_ambient[r].w + far_lamps[r]);
            }
            auto& on_far = lights[far];
            if (on_far.size() < render::k_door_lights) {
                on_far.push_back(into_far);
                leaf = uint32_t(on_far.size());
            }
            auto& on_near = lights[near];
            if (on_near.size() < render::k_door_lights) {
                on_near.push_back(into_near);
                near_leaf = uint32_t(on_near.size());
            }
        }
        s.leaves.clear();
        for (const oda::Part& part : s.anim->parts) {
            const auto& track = s.closing ? part.close : part.open;
            const float at = s.time * oda::k_sample_rate;
            const size_t k = std::min(size_t(at), track.size() - 1), n = std::min(k + 1, track.size() - 1);
            const auto quat = [](const oda::Pose& p) {
                return glm::quat(p.rotation[3], p.rotation[0], p.rotation[1], p.rotation[2]);
            };
            const float w = at - float(k);
            const glm::quat q = glm::slerp(quat(track[k]), quat(track[n]), w);
            const glm::vec3 t = glm::mix(glm::vec3(track[k].translation[0], track[k].translation[1], track[k].translation[2]),
                                         glm::vec3(track[n].translation[0], track[n].translation[1], track[n].translation[2]), w);
            const glm::mat3 rotation = s.rotation * glm::mat3_cast(q);
            const glm::vec3 position = s.position + s.rotation * t;
            s.leaves.push_back({rotation, position, glm::vec3(part.lo[0], part.lo[1], part.lo[2]),
                                glm::vec3(part.hi[0], part.hi[1], part.hi[2])});
            // A box of it in each space it stands in.
            const DoorSwing::Leaf& box = s.leaves.back();
            const float scale = glm::length(rotation[0]);
            const glm::vec3 half = (box.hi - box.lo) * 0.5f * scale, middle = rotation * ((box.lo + box.hi) * 0.5f);
            if (physics::World* w = physics_of(near)) {
                solid[w].push_back({position + middle, glm::quat_cast(rotation / scale), half});
            }
            if (physics::World* w = physics_of(far); w && carried) {
                solid[w].push_back({s.to_partner.point(position + middle), glm::quat_cast(rotate(rotation) / scale), half});
            }
            for (uint32_t submesh : part.submeshes) {
                if (near) {
                    shapes[near].push_back({s.anim->mesh, submesh, rotation, position, near_leaf});
                }
                // The leaf swings through the doorway: past its plane it is in the far space. A gate's far side is this
                // space, where a copy would stand a doorway's depth along the leaf.
                if (carried) {
                    shapes[far].push_back({s.anim->mesh, submesh, rotate(rotation), s.to_partner.point(position), leaf});
                }
            }
            // The game's gates are two doors, each modelled on its outer face with a bare slab behind, for it never opens
            // them: seen open, a leaf needs the face the other door shows there. That door's leaf on the same side rides
            // this one, back to back.
            if (near && near == far && s.gate && s.partner_anim) {
                // The pose it opens from, closing too: a close track starts with the door open.
                const oda::Pose& closed = part.open.front();
                const glm::mat3 shut = s.rotation * glm::mat3_cast(quat(closed));
                const glm::vec3 shut_at =
                    s.position + s.rotation * glm::vec3(closed.translation[0], closed.translation[1], closed.translation[2]);
                const glm::vec3 lo(part.lo[0], part.lo[1], part.lo[2]), hi(part.hi[0], part.hi[1], part.hi[2]);
                const glm::vec3 middle = shut_at + shut * ((lo + hi) * 0.5f);
                const oda::Part* other = nullptr;
                float nearest = 1e30f;
                for (const oda::Part& o : s.partner_anim->parts) {
                    const glm::vec3 c = s.partner_position + s.partner_rotation * (glm::vec3(o.lo[0], o.lo[1], o.lo[2]) +
                                                                                   glm::vec3(o.hi[0], o.hi[1], o.hi[2])) * 0.5f;
                    if (const float d = glm::distance(c, middle); d < nearest) {
                        nearest = d;
                        other = &o;
                    }
                }
                if (other) {
                    // How far along the doorway's normal a box reaches, each way.
                    const auto span = [&](const glm::mat3& m, const glm::vec3& at, const glm::vec3& a, const glm::vec3& b) {
                        glm::vec2 out(1e30f, -1e30f);
                        for (int i = 0; i < 8; ++i) {
                            const glm::vec3 corner(i & 1 ? b.x : a.x, i & 2 ? b.y : a.y, i & 4 ? b.z : a.z);
                            const float along = glm::dot(at + m * corner, s.near.normal);
                            out = glm::vec2(std::min(out.x, along), std::max(out.y, along));
                        }
                        return out;
                    };
                    const glm::vec2 mine = span(shut, shut_at, lo, hi);
                    const glm::vec2 theirs = span(s.partner_rotation, s.partner_position,
                                                  glm::vec3(other->lo[0], other->lo[1], other->lo[2]),
                                                  glm::vec3(other->hi[0], other->hi[1], other->hi[2]));
                    const glm::vec3 flush = s.near.normal * (mine.x - theirs.y);
                    const glm::mat3 moved = (rotation / glm::length(rotation[0])) * glm::inverse(shut / glm::length(shut[0]));
                    const glm::mat3 back_rotation = moved * s.partner_rotation;
                    const glm::vec3 back_position = position + moved * (s.partner_position + flush - shut_at);
                    for (uint32_t submesh : other->submeshes) {
                        shapes[near].push_back({s.partner_anim->mesh, submesh, back_rotation, back_position, near_leaf});
                    }
                }
            }
        }
        for (uint32_t submesh : s.anim->still) {
            if (near) {
                shapes[near].push_back({s.anim->mesh, submesh, s.rotation, s.position});
            }
        }
        // Behind a door with nothing beyond it, what stands there stays.
        for (uint32_t submesh : s.anim->backing) {
            if (near && !s.partner) {
                shapes[near].push_back({s.anim->mesh, submesh, s.rotation, s.position});
            }
        }
        if (far && s.partner_anim) {
            for (uint32_t submesh : s.partner_anim->still) {
                shapes[far].push_back({s.partner_anim->mesh, submesh, s.partner_rotation, s.partner_position});
            }
        }
    }
    static const std::vector<Space::DoorShape> none;
    static const std::vector<render::DoorLight> no_lights;
    const auto apply = [&](Space* space) {
        if (!space->doors_turned()) {
            turn_doors(*space);
        }
        for (const Space::TurnedDoor& t : space->turned_doors()) {
            if (!find_swing(t.form_id)) {
                auto& list = shapes[space];
                list.insert(list.end(), t.shapes.begin(), t.shapes.end());
            }
        }
        auto it = shapes.find(space);
        space->set_door_shapes(it == shapes.end() ? none : it->second);
        auto l = lights.find(space);
        space->set_door_lights(l == lights.end() ? no_lights : l->second);
    };
    if (m_space) {
        apply(m_space.get());
    }
    for (auto& o : m_others) {
        if (o->space) {
            apply(o->space.get());
        }
    }
    static const std::vector<physics::World::Leaf> no_leaves;
    const auto place = [&](physics::World* w) {
        if (w) {
            auto it = solid.find(w);
            w->set_leaves(it == solid.end() ? no_leaves : it->second);
        }
    };
    place(m_physics.get());
    for (auto& o : m_others) {
        place(o->physics.get());
    }
}

physics::World* App::physics_of(const Space* space) {
    if (!space) {
        return nullptr;
    }
    if (space == m_space.get()) {
        return m_physics.get();
    }
    for (auto& o : m_others) {
        if (o->space.get() == space) {
            return o->physics.get();
        }
    }
    return nullptr;
}

void App::load_door_animations() {
    const auto file = m_opt.cache / "rules" / "doors.oda";
    if (!std::filesystem::exists(file)) {
        log::info("no {}: doors don't move", file.string());
        return;
    }
    for (oda::Door& d : oda::read(read_file(file)).doors) {
        m_door_anims.emplace(d.base_form_id, std::move(d));
    }
    log::info("doors: {} animated", m_door_anims.size());
}

// The image space this frame's views share, the door's view with the main one: this space's, going over to the other
// side's as the camera nears an open doorway, half and half on its plane. The other side's camera stands at the same
// weight there, so a crossing changes nothing. The two blend as the game blends image spaces, over the leading block
// (HDR, cinematic, tint, depth of field); the modifiers on top are the player's own, the same on both sides.
std::optional<render::ImageSpaceData> App::door_grade(const glm::vec3& camera) {
    if (!m_space || !m_space->environment().sky.image_space_valid) {
        return std::nullopt;
    }
    render::ImageSpaceData grade = m_space->environment().sky.image_space;
    float best = 0.0f;
    const render::ImageSpaceData* other = nullptr;
    for (const DoorPortal& p : m_doors) {
        if (!p.open || !p.seamless || p.gate) {
            continue;
        }
        const float side = p.near.side(camera);
        if (side < 0.0f || side >= k_grade_m || !p.near.within(camera - p.near.normal * side, k_grade_m)) {
            continue;
        }
        const float t = side / k_grade_m;
        const float w = 0.5f * (1.0f - t * t * (3.0f - 2.0f * t));
        const OtherSpace* o = p.same_space ? nullptr : other_space(m_space->world().doors[p.index].destination_space, false);
        Space* far = p.same_space ? m_space.get() : o && o->space ? o->space.get() : nullptr;
        if (w > best && far && far->environment().sky.image_space_valid) {
            best = w;
            other = &far->environment().sky.image_space;
        }
    }
    if (other) {
        float* to = &grade.eye_adapt_speed;
        const float* from = &other->eye_adapt_speed;
        for (int i = 0; i < 13; ++i) { // hdr, cinematic, tint amount: contiguous
            to[i] += (from[i] - to[i]) * best;
        }
        grade.tint += (other->tint - grade.tint) * best;
        grade.dof += (other->dof - grade.dof) * best;
    }
    return grade;
}

// A crossing's game side, once the camera is clear of the door.
void App::settle_travel() {
    const Travel t = *m_travel;
    m_travel.reset();
    log::info("travel from {} settled", t.from);
    if (m_world_sim) {
        if (t.left_cell) {
            m_world_sim->cell_detached(t.left_cell);
        }
        int32_t cx = 0, cy = 0;
        const WorldSim::Place place = player_place(cx, cy);
        m_world_sim->player_moved(place, cx, cy);
    }
    m_autosave_after_move = m_user.number("bSaveOnTravel:SaveGame") > 0.5;
}

void App::cross_door(const DoorPortal& door) {
    const auto started = Clock::now();
    release_grab(); // its body is in the space being left
    const osw::Door d = m_space->world().doors[door.index];
    const PortalTransform t = door.to_far;
    const std::string left = path::to_lower(m_space->edid());
    // Straight back into the space a crossing left: that crossing never happened, for the game.
    const bool back = !door.same_space && m_travel && m_travel->from == path::to_lower(d.destination_space);
    if (m_travel && !back) {
        settle_travel();
    }
    const uint32_t left_cell =
        !door.same_space && m_space->interior() && !m_space->world().cells.empty() ? m_space->world().cells.front().form_id : 0;
    // Third person, the camera and the body cross apart: it joins the body gone ahead, or leaves it behind backing through.
    const bool third = m_walk && m_player && m_player->view() == View::third_person && !m_mount.ref;
    const bool joins = m_body_apart && !door.same_space && m_body_apart->space == path::to_lower(d.destination_space);
    const bool leaves = third && !m_body_apart && !door.same_space && door.near.side(m_player->feet()) > 0.0f;
    if (m_body_apart && !joins) {
        body_back();
    }
    const bool carry_body = !joins && !leaves;
    if (!door.same_space) {
        OtherSpace* other = other_space(d.destination_space, false);
        if (!other || !other->space) {
            return;
        }
        std::swap(m_space, other->space);
        std::swap(m_physics, other->physics);
        std::swap(m_smooth_refs, other->smooth);
        m_smooth_space = m_space.get();
        // The effects' lights go with the camera this frame: the effects set them again only after this frame's lights.
        std::array<render::PointLight, render::k_moving_lights> lights{};
        size_t n = 0;
        for (const render::PointLight& l : other->space->moving_lights()) {
            if (l.flags & render::point_light_on) {
                lights[n] = l;
                lights[n++].position = t.point(l.position);
            }
        }
        m_space->set_moving_lights(std::span(lights).first(n));
        other->space->set_moving_lights({});
        // The view through the door had the far space's water ready: it is the main view's now (Renderer::cross_view swaps
        // their images likewise). Made again from the world it costs 7 ms.
        std::swap(m_water_system, m_portal_water);
        // Its effects too: fires and smoke burn on as they were, where a new start would light them again from nothing.
        if (m_world_fx && m_portal_fx) {
            std::swap(m_world_fx, m_portal_fx);
            m_portal_fx->hand_over_tracked(*m_world_fx, m_camera.position, k_carried_particles_m);
            m_portal_fx->drop_tracked();
        }
        if (carry_body) {
            other->space->hand_player_to(*m_space, [&](const glm::vec3& p) { return t.point(p); }, t.angle); // its weapon as it was
        }
        other->name = left;
        other->used = m_door_frame;
    }
    if (joins) {
        m_body_apart.reset();
    } else if (leaves) {
        m_body_apart = BodyApart{left, t, door.near};
    }
    m_camera.position = t.point(m_camera.position);
    m_camera.yaw = t.yaw(m_camera.yaw);
    if (m_executor) {
        m_executor->carry([&](const glm::vec3& p) { return t.point(p); }, t.angle);
    }
    if (m_walk && m_player && carry_body) {
        if (!m_physics) {
            m_physics = std::make_unique<physics::World>(m_opt.cache, m_space->world_ptr());
        }
        if (m_mount.ref) {
            dismounted(); // off at once, without the animation
        }
        // The camera as this frame placed it, carried above: the player's own would drop the eye's lag and its animated
        // node, and the frame would show the eye 27 cm ahead.
        m_player->carry(*m_physics, m_space->world_ptr(), [&](const glm::vec3& p) { return t.point(p); },
                        t.angle);
    }
    if (!door.same_space) {
        if (!m_space->interior()) {
            m_interior_sky.clear(); // the next visit draws again
        }
        rebuild_doors();
        apply_destruction();
    }
    // The door just come out of stays open behind the camera.
    for (auto& p : m_doors) {
        if (m_space->world().doors[p.index].form_id == d.destination) {
            set_door_open(p, true, true);
        }
    }
    m_door_last_camera = m_camera.position;
    if (back) {
        m_travel.reset();
        log::info("straight back: no travel");
    } else if (!door.same_space) {
        m_travel = Travel{d.destination, left, left_cell};
    }
    ++m_crossings;
    m_renderer->cross_view();
    log::info("crossed door {:08X}: {} -> {}, {:.2f} ms", d.form_id, left, m_space->edid(), ms_between(started, Clock::now()));
}

// Where the camera's ray meets a moving door, nearest of its parts.
std::optional<float> App::leaf_hit(const DoorSwing& s) const {
    glm::vec3 from = m_camera.position, dir = m_camera.forward();
    if (path::to_lower(m_space->edid()) != s.space) {
        const PortalTransform back = s.to_partner.inverse();
        from = back.point(from);
        dir = back.vector(dir);
    }
    std::optional<float> best;
    for (const DoorSwing::Leaf& leaf : s.leaves) {
        const glm::mat3 to_mesh = glm::inverse(leaf.rotation);
        const glm::vec3 o = to_mesh * (from - leaf.position), d = to_mesh * dir;
        float t0 = 0.0f, t1 = 1e30f;
        for (int c = 0; c < 3; ++c) {
            if (std::abs(d[c]) < 1e-8f) {
                t1 = o[c] < leaf.lo[c] || o[c] > leaf.hi[c] ? -1.0f : t1;
                continue;
            }
            const float a = (leaf.lo[c] - o[c]) / d[c], b = (leaf.hi[c] - o[c]) / d[c];
            t0 = std::max(t0, std::min(a, b));
            t1 = std::min(t1, std::max(a, b));
        }
        if (t0 <= t1 && (!best || t0 < *best)) {
            best = t0;
        }
    }
    return best;
}

App::DoorPortal* App::aimed_door(float& distance) {
    const glm::vec3 eye = m_camera.position, dir = m_camera.forward();
    DoorPortal* best = nullptr;
    float best_t = k_activate_m;
    for (auto& p : m_doors) {
        // A door that has moved is where it has moved to, not in its doorway.
        if (const DoorSwing* swing = find_swing(m_space->world().doors[p.index].form_id)) {
            if (const auto t = leaf_hit(*swing); t && *t <= best_t) {
                best = &p;
                best_t = *t;
            }
            continue;
        }
        const float facing = glm::dot(p.near.normal, dir);
        if (facing >= -1e-3f) {
            continue; // looking away from it, or along it
        }
        const float t = p.near.side(eye) / -facing;
        if (t < 0.0f || t > best_t || !p.near.within(eye + dir * t, 0.05f)) {
            continue;
        }
        best = &p;
        best_t = t;
    }
    distance = best_t;
    return best;
}

uint32_t App::aimed_hidden_door(float& distance) {
    if (!m_space) {
        return 0;
    }
    if (const DoorPortal* p = aimed_door(distance)) {
        return m_space->world().doors[p->index].form_id;
    }
    const osw::Ref* room = aimed_room_door();
    if (!room || !find_swing(room->form_id)) {
        return 0;
    }
    distance = glm::dot(glm::vec3(room->position[0], room->position[1], room->position[2]) - m_camera.position,
                        m_camera.forward());
    return room->form_id;
}

bool App::door_open(uint32_t form_id) {
    const DoorSwing* swing = find_swing(form_id);
    return swing && !swing->closing;
}

// A door with nothing to open takes the player through the loading screen to its XTEL, 10 units up, with its
// open sound.
bool App::travel_door(uint32_t form_id) {
    if (!m_space) {
        return false;
    }
    for (const DoorPortal& p : m_doors) {
        const osw::Door& d = m_space->world().doors[p.index];
        if (d.form_id != form_id || !p.travels) {
            continue;
        }
        if (m_game && m_audio) {
            if (const forms::FormEntry* base = m_game->db().find(d.base_form_id)) {
                if (const auto sound = m_game->db().field(*base, forms::sig("SNAM"))) {
                    if (auto descriptor = m_audio->find_descriptor(std::format("{:08X}", sound->as<uint32_t>()))) {
                        m_audio->play(*descriptor, glm::vec3(d.position[0], d.position[1], d.position[2]));
                    }
                }
            }
        }
        Pose pose;
        pose.position_units =
            glm::vec3(d.arrival_position[0], d.arrival_position[1], d.arrival_position[2]) / units::k_skyrim_unit_m;
        pose.position_units.z += 10.0f;
        pose.heading_deg = glm::degrees(d.arrival_rotation[2]);
        const std::string destination = d.destination_space;
        travel(destination, pose);
        return true;
    }
    return false;
}

void App::activate_door() {
    float distance = 0.0f;
    if (DoorPortal* best = aimed_door(distance)) {
        if (best->travels) {
            travel_door(m_space->world().doors[best->index].form_id);
            return;
        }
        const DoorSwing* swing = find_swing(m_space->world().doors[best->index].form_id);
        set_door_open(*best, !best->open || (swing && swing->closing), true);
    } else {
        activate_room_door();
    }
}

// A door between rooms: what the camera points at within reach, stood at its middle. It stays as it is left, and nothing
// of it collides while it is open.
const osw::Ref* App::aimed_room_door() {
    const glm::vec3 eye = m_camera.position, dir = m_camera.forward();
    const auto& refs = m_space->world().refs;
    const osw::Ref* best = nullptr;
    float best_d = k_room_door_m;
    for (uint32_t index : m_room_doors) {
        const osw::Ref& r = refs[index];
        if (const DoorSwing* swing = find_swing(r.form_id)) {
            if (const auto t = leaf_hit(*swing); t && *t <= k_activate_m) {
                return &r;
            }
            continue;
        }
        const glm::vec3 centre = glm::vec3(r.position[0], r.position[1], r.position[2]) + glm::vec3(0, 0, 1.2f);
        const float along = glm::dot(centre - eye, dir);
        if (along < 0.0f || along > k_activate_m) {
            continue;
        }
        const float d = glm::length(centre - (eye + dir * along));
        if (d < best_d) {
            best_d = d;
            best = &r;
        }
    }
    return best;
}

bool App::activate_room_door() {
    const osw::Ref* best = m_space ? aimed_room_door() : nullptr;
    return best && swing_room_door(*best);
}

// ObjectReference.SetOpen on a door between rooms: as if someone had opened it.
void App::script_door(uint32_t form_id, bool open) {
    if (!m_space) {
        return;
    }
    const auto& refs = m_space->world().refs;
    for (const uint32_t index : m_room_doors) {
        if (refs[index].form_id == form_id) {
            const DoorSwing* swing = find_swing(form_id);
            if (open == (!swing || swing->closing)) {
                swing_room_door(refs[index]);
            }
            return;
        }
    }
    // A gate with no door of ours to swing: the model's own Open and Close.
    if (play_gamebryo(form_id, open ? "Open" : "Close", true, 0.0f) == 0 && m_physics) {
        m_physics->set_ref_solid(form_id, !open);
    }
}

bool App::swing_room_door(const osw::Ref& door) {
    const osw::Ref* best = &door;
    const std::string space = path::to_lower(m_space->edid());
    if (DoorSwing* s = find_swing(best->form_id)) {
        const oda::Part& p = s->anim->parts[0];
        const auto seconds = [](const std::vector<oda::Pose>& t) { return float(t.size() - 1) / oda::k_sample_rate; };
        const float from = seconds(s->closing ? p.close : p.open), to = seconds(s->closing ? p.open : p.close);
        s->time = to * (1.0f - std::min(s->time / std::max(from, 1e-3f), 1.0f));
        s->closing = !s->closing;
        play_door_sound(s->anim, !s->closing, s->position);
        return true;
    }
    DoorSwing s;
    s.door = best->form_id;
    s.space = space;
    s.anim = &m_door_anims.at(best->base_form_id);
    s.rotation = ref_rotation(best->rotation, best->scale);
    s.position = glm::vec3(best->position[0], best->position[1], best->position[2]);
    m_space->set_ref_hidden(s.door, true);
    if (m_physics) {
        m_physics->set_ref_solid(s.door, false);
    }
    play_door_sound(s.anim, true, s.position);
    m_swings.push_back(std::move(s));
    return true;
}

void App::finish_space_loads() {
    for (auto& o : m_others) {
        if (o->loading.valid() && o->loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                o->space = o->loading.get();
                const auto start = Clock::now();
                o->space->finish(); // what only the render thread may do: its land textures
                watch_cells(*o->space);
                log::info("space {}: ready behind a door, {:.2f} ms on the main thread", o->name,
                          ms_between(start, Clock::now()));
            } catch (const std::exception& e) {
                log::warn("space {}: {}", o->name, e.what());
            }
        }
    }
}

// Where the floor stands just in front of a doorway, against the doorway's bottom.
std::optional<float> App::floor_offset(physics::World* world, const Doorway& door) const {
    if (!world) {
        return std::nullopt;
    }
    const glm::vec3 at = door.bottom() + door.normal * k_floor_sample_m;
    if (!world->cell_loaded(at)) {
        return std::nullopt;
    }
    const auto hit = world->raycast_shared(at + glm::vec3(0, 0, k_floor_reach_m), glm::vec3(0, 0, -1), 2.0f * k_floor_reach_m);
    if (!hit) {
        return std::nullopt;
    }
    return k_floor_reach_m - hit->distance;
}

// A doorway's bottom is the door model's, and the ground meets it only so well: up to 40 cm out where a door isn't part of
// a kit. The far side is carried by what the floors say, so the ground runs on through the doorway at one height.
void App::join_floors(DoorPortal& door, physics::World* far) {
    if (door.floored || !far || !m_physics || far->busy() || m_physics->busy()) {
        return;
    }
    const auto near_floor = floor_offset(m_physics.get(), door.near), far_floor = floor_offset(far, door.far);
    if (near_floor && far_floor) {
        door.to_far.from.z = door.near.bottom().z + *near_floor;
        door.to_far.to.z = door.far.bottom().z + *far_floor;
        door.floored = true;
    }
}

Space* App::body_space() {
    if (!m_body_apart) {
        return m_space.get();
    }
    OtherSpace* o = other_space(m_body_apart->space, false);
    return o && o->space ? o->space.get() : nullptr;
}

glm::vec3 App::player_feet_main() const {
    const glm::vec3 feet = m_player->feet();
    return m_body_apart ? m_body_apart->to_main.point(feet) : feet;
}

// The body through a doorway the camera hasn't reached: into the far space's collision and actors.
void App::body_through(const DoorPortal& door) {
    const osw::Door& d = m_space->world().doors[door.index];
    OtherSpace* o = other_space(d.destination_space, false);
    if (!o || !o->space || !o->physics) {
        return;
    }
    const PortalTransform t = door.to_far;
    release_grab();
    m_player->carry(*o->physics, o->space->world_ptr(), [&](const glm::vec3& p) { return t.point(p); }, t.angle);
    m_space->hand_player_to(*o->space, [&](const glm::vec3& p) { return t.point(p); }, t.angle);
    m_body_apart = BodyApart{o->name, t.inverse(), door.far};
    log::info("body through door {:08X} ahead of the camera", d.form_id);
}

// ... and back into the camera's space.
void App::body_back() {
    Space* from = body_space();
    if (!m_body_apart || !from || !m_physics) {
        m_body_apart.reset();
        return;
    }
    const PortalTransform t = m_body_apart->to_main;
    release_grab();
    m_player->carry(*m_physics, m_space->world_ptr(), [&](const glm::vec3& p) { return t.point(p); }, t.angle);
    from->hand_player_to(*m_space, [&](const glm::vec3& p) { return t.point(p); }, t.angle);
    m_body_apart.reset();
}

// The third-person camera's collision goes on through the doorway nearest the body into the space beyond it.
void App::place_camera_doorway() {
    if (!m_player) {
        return;
    }
    if (m_body_apart) {
        const Doorway& w = m_body_apart->way;
        const PortalTransform& t = m_body_apart->to_main;
        m_player->set_camera_doorway(Player::CameraDoorway{w.centre, w.normal, w.half_width, w.half_height, m_physics.get(),
                                                           t.from, t.to, t.angle});
        return;
    }
    const glm::vec3 feet = m_player->feet();
    const DoorPortal* best = nullptr;
    float best_d = k_camera_doorway_m;
    for (const DoorPortal& p : m_doors) {
        if (p.open && p.seamless && !p.gate && !p.same_space) {
            if (const float dist = glm::length(feet - p.near.bottom()); dist < best_d) {
                best = &p;
                best_d = dist;
            }
        }
    }
    OtherSpace* o = best ? other_space(m_space->world().doors[best->index].destination_space, false) : nullptr;
    if (!o || !o->physics) {
        m_player->set_camera_doorway(std::nullopt);
        return;
    }
    m_player->set_camera_doorway(Player::CameraDoorway{best->near.centre, best->near.normal, best->near.half_width,
                                                       best->near.half_height, o->physics.get(), best->to_far.from,
                                                       best->to_far.to, best->to_far.angle});
}

void App::update_doors(float time) {
    update_swings(time);
    ++m_door_frame;
    finish_space_loads();
    if (!m_space) {
        return;
    }
    const glm::vec3 cam = m_camera.position;

    // Third person: the body crosses a doorway on its own when its feet do.
    const bool third = m_walk && m_player && m_player->view() == View::third_person && !m_mount.ref;
    if (m_body_apart && (!third || !body_space())) {
        body_back();
    }
    if (third) {
        const glm::vec3 feet = m_player->feet();
        if (std::isfinite(m_door_last_feet.x)) {
            if (m_body_apart) {
                const Doorway& w = m_body_apart->way;
                const float a = w.side(m_door_last_feet), b = w.side(feet);
                if (a > 0.0f && b <= 0.0f && w.within(m_door_last_feet + (feet - m_door_last_feet) * (a / (a - b)), 0.05f)) {
                    body_back();
                }
            } else {
                for (const auto& p : m_doors) {
                    if (!p.open || !p.seamless || p.gate || p.same_space) {
                        continue;
                    }
                    const float a = p.near.side(m_door_last_feet), b = p.near.side(feet);
                    if (a > 0.0f && b <= 0.0f && p.near.within(m_door_last_feet + (feet - m_door_last_feet) * (a / (a - b)), 0.05f)) {
                        body_through(p);
                        break;
                    }
                }
            }
        }
        m_door_last_feet = m_player->feet();
    } else {
        m_door_last_feet = glm::vec3(std::numeric_limits<float>::quiet_NaN());
    }
    place_camera_doorway();

    // SPUD_RECORD_DOORS=<directory>: each walk up to an open doorway saves the frames that follow, for what only a player's
    // screen shows. Again once the camera and body have been 2.5 m clear of every doorway.
    if (static const char* record = std::getenv("SPUD_RECORD_DOORS"); record && *record) {
        // The camera or the body, whichever is nearer: in third person the body is there first.
        float nearest = 1e30f;
        const glm::vec3 feet = m_player ? player_feet_main() : cam;
        for (const auto& p : m_doors) {
            for (const glm::vec3& at : {cam, feet}) {
                if (p.open && p.seamless && !p.gate && p.near.within(at - p.near.normal * p.near.side(at), 0.5f)) {
                    nearest = std::min(nearest, std::abs(p.near.side(at)));
                }
            }
        }
        if (nearest > 2.5f) {
            m_record_armed = true;
        } else if (nearest < 1.5f && m_record_armed && m_record_left == 0) {
            m_record_armed = false;
            m_record_left = 45;
            m_record_index = 0;
            m_record_dir = std::filesystem::path(record) / std::format("{:03d}_{}", m_record_walks++, m_space->edid());
            log::info("recording 45 frames to {}", m_record_dir.string());
        }
    }

    // Clear of the door it came out of, in front of it or off to the side, the crossing is settled.
    if (m_travel) {
        const auto came = std::ranges::find_if(m_doors, [&](const DoorPortal& p) {
            return m_space->world().doors[p.index].form_id == m_travel->door;
        });
        if (came == m_doors.end() || came->near.side(cam) > k_travel_clear_m ||
            !came->near.within(cam - came->near.normal * came->near.side(cam), k_travel_clear_m)) {
            settle_travel();
        }
    }

    // Through an open doorway: the camera's step crossed its plane inside the opening.
    if (std::isfinite(m_door_last_camera.x)) {
        for (const auto& p : m_doors) {
            if (!p.open || !p.seamless || p.gate) {
                continue;
            }
            const float a = p.near.side(m_door_last_camera), b = p.near.side(cam);
            if (a > 0.0f && b <= 0.0f) {
                const glm::vec3 at = m_door_last_camera + (cam - m_door_last_camera) * (a / (a - b));
                if (p.near.within(at, 0.05f)) {
                    const DoorPortal crossing = p;
                    cross_door(crossing);
                    return;
                }
            }
        }
    }
    m_door_last_camera = cam;

    if (m_physics) {
        // Each open doorway in this space's collision, and its other side in the far space's.
        std::vector<physics::World::Opening> openings;
        std::unordered_map<physics::World*, std::vector<physics::World::Opening>> far_openings;
        for (auto& o : m_others) {
            if (o->physics) {
                far_openings[o->physics.get()];
            }
        }
        for (const auto& p : m_doors) {
            if (p.open && p.seamless && !p.gate) {
                openings.push_back({p.near.centre, p.near.normal, p.near.half_width, p.near.half_height, k_passage_m, k_passage_m});
                OtherSpace* o = p.same_space ? nullptr : other_space(m_space->world().doors[p.index].destination_space, false);
                if (o && o->physics) {
                    far_openings[o->physics.get()].push_back(
                        {p.far.centre, p.far.normal, p.far.half_width, p.far.half_height, k_passage_m, k_passage_m});
                }
            }
        }
        m_physics->set_openings(openings);
        for (auto& [world, list] : far_openings) {
            world->set_openings(list);
        }
    }

    // Reach: read what near doors lead to, open and close them.
    struct Stream {
        OtherSpace* other;
        DoorPortal* door;
        float distance;
    };
    std::vector<Stream> streams;
    for (auto& p : m_doors) {
        if (p.faces_camera && !p.open && p.near.side(cam) < 0.0f) {
            p.near.normal = -p.near.normal;
            p.near.right = -p.near.right;
            p.to_far = portal_transform(p.near, p.far);
            p.floored = false;
        }
        const float dist = glm::length(cam - p.near.centre);
        if ((p.automatic || (!m_walk && m_doors_auto)) && dist < k_auto_open_m && p.near.side(cam) > -0.25f) {
            set_door_open(p, true, false);
        } else if (p.open && dist > k_close_m &&
                   (p.gate || glm::length(p.to_far.point(cam) - p.far.centre) > k_close_m)) {
            set_door_open(p, false, false);
        }
        if (p.same_space && p.seamless && !p.gate && dist < k_physics_m) {
            join_floors(p, m_physics.get());
        }
        if (p.same_space || !p.seamless || (dist > k_preload_m && !p.open)) {
            continue;
        }
        OtherSpace* o = other_space(m_space->world().doors[p.index].destination_space, true);
        if (!o->space) {
            continue;
        }
        auto it = std::find_if(streams.begin(), streams.end(), [&](const Stream& s) { return s.other == o; });
        if (it == streams.end()) {
            streams.push_back({o, &p, dist});
        } else if (dist < it->distance) {
            *it = {o, &p, dist};
        }
    }
    // The far spaces stream from the camera carried through their nearest door.
    for (size_t i = 0; i < streams.size(); ++i) {
        const Stream& s = streams[i];
        const PortalTransform& t = s.door->to_far;
        Space& sp = *s.other->space;
        const glm::vec3 far_cam = t.point(cam);
        // Behind a shut door a space only has to be there: it streams, nothing of it is seen, so a load goes at a
        // quarter of the pace (all at once it slows every part of the near space's frame). Idle ones take turns, each
        // on a frame of its own: together they would be a spike every so many frames.
        if (s.door->open || (sp.busy() && (m_door_frame + i) % 4 == 0) ||
            (m_door_frame + i * 16) % k_idle_stream_frames == 0) {
            ActorSet* actors = sp.actors();
            if (actors) {
                // On the world's tick, as this space's own actors: without its step their graphs and feet stood at a step
                // of nothing, and their bodies didn't follow the frames between ticks.
                actors->set_between_ticks(m_world_still);
                actors->set_time(m_actor_dt);
                actors->set_hold(m_hold_actors);
                actors->set_grid_radius(m_grids_to_load / 2);
            }
            sp.set_actor_blend(m_fixed_step > 0.0 ? (m_world_still ? m_shown_fraction : m_tick_blend) : 1.0f);
            sp.update(far_cam, t.vector(m_camera.forward()), m_space_settings, m_render_settings.terrain);
            if (actors) {
                actors->set_between_ticks(false);
            }
        }
        if (s.door->open) {
            evaluate_environment(sp, 0.0f);
            // As the frame's own space (App::run_frame): region emittance colours its lights.
            if (m_sky) {
                for (uint32_t w : sp.emittance_weathers()) {
                    if (auto c = m_sky->weather_colour(w, osa::colour_effect_lighting)) {
                        sp.set_emittance(w, *c);
                    }
                }
            }
            sp.update_lights(far_cam, time, m_space_settings);
        }
        if (m_walk && m_player && (s.door->open || s.distance < k_physics_m)) {
            if (!s.other->physics) {
                s.other->physics = std::make_unique<physics::World>(m_opt.cache, sp.world_ptr());
                for (const auto& p : m_doors) {
                    const osw::Door& d = m_space->world().doors[p.index];
                    if (p.open && d.destination_space == s.other->name) {
                        s.other->physics->set_ref_solid(d.destination, false);
                    }
                }
            }
            if (!m_body_apart) { // apart, the body's own tick streams its space
                s.other->physics->update(t.point(m_player->feet()));
            }
            join_floors(*s.door, s.other->physics.get());
        }
    }
    retire_spaces();
}

void App::retire_spaces() {
    // Exteriors stay (their terrain costs tens of milliseconds to make again). An interior goes through a retirement
    // instead of a teardown, which would wait for the GPU: its cells unload now and the rest goes when no frame in
    // flight can still use it.
    size_t interiors = 0;
    OtherSpace* oldest = nullptr;
    for (auto& o : m_others) {
        if (o->space && o->space->interior()) {
            ++interiors;
            if (!oldest || o->used < oldest->used) {
                oldest = o.get();
            }
        }
    }
    if (interiors > k_kept_interiors && oldest->used + 1 < m_door_frame && !oldest->loading.valid()) {
        m_offered.erase(oldest->space.get()); // its address may come again for another space
        oldest->space->retire();
        m_retired.push_back({std::move(oldest->physics), std::move(oldest->space), m_door_frame + k_retire_frames});
        std::erase_if(m_others, [&](const std::unique_ptr<OtherSpace>& o) { return o.get() == oldest; });
    }
    // A space goes without a drain, so what is still uploading for it has to finish first.
    std::erase_if(m_retired, [&](const RetiredSpace& r) { return r.free_at <= m_door_frame && !r.space->in_flight(); });
}

void App::pick_portal(const render::View& view) {
    m_portal_door = -1;
    if (!m_space) {
        return;
    }
    float best = k_portal_m;
    for (size_t i = 0; i < m_doors.size(); ++i) {
        const DoorPortal& p = m_doors[i];
        if (!p.open || !p.seamless || p.gate || p.near.side(view.position) <= 0.0f) {
            continue;
        }
        const float dist = glm::length(view.position - p.near.centre);
        OtherSpace* o = p.same_space ? nullptr : other_space(m_space->world().doors[p.index].destination_space, false);
        if (dist >= best || (!p.same_space && (!o || !o->space)) || !quad_in_view(view, p.near)) {
            continue;
        }
        best = dist;
        m_portal_door = int(i);
    }
    if (m_portal_door < 0) {
        return;
    }
    const DoorPortal& p = m_doors[size_t(m_portal_door)];
    const PortalTransform& t = p.to_far;
    m_portal_view = render::View::make(t.point(view.position), t.yaw(m_camera.yaw), m_camera.pitch, view.fov_y,
                                       view.aspect, view.near_plane);
    // The far view stops at the far doorway. Culling takes it as the near plane; the picture clips there, the projection
    // stays plain, so everything that reads the far frame's depth reads it as any other.
    render::View oblique = m_portal_view;
    if (!set_near_plane(oblique, p.far.centre, p.far.normal)) {
        m_portal_door = -1;
        return;
    }
    m_portal_view.frustum = oblique.frustum;
    narrow_to_doorway(m_portal_view, p.far);
    // A leaf through the doorway is cut twice along one line, the near half by the doorway's depth, the far half here: the
    // far frame runs on a little past the plane, beyond what the stamp's tolerance takes (portal_stamp.frag), or the room
    // behind shows through the cut.
    const float past = 0.005f * glm::length(view.position - p.near.centre) + 0.002f;
    m_portal_view.clip_plane =
        glm::vec4(p.far.normal, past - glm::dot(p.far.normal, p.far.centre - m_portal_view.position));
}

const render::ParticleFrame* App::carried_particles(const PortalTransform& to_far) {
    const render::ParticleFrame* own = m_portal_fx ? m_portal_fx->frame() : nullptr;
    const render::ParticleFrame* main = m_world_fx ? m_world_fx->frame() : nullptr;
    if (!main) {
        return own;
    }
    m_portal_particles.clear();
    m_portal_systems.clear();
    m_portal_particle_draws.clear();
    if (own) {
        m_portal_particles.assign(own->particles.begin(), own->particles.end());
        m_portal_systems.assign(own->systems.begin(), own->systems.end());
        m_portal_particle_draws.assign(own->draws.begin(), own->draws.end());
    }
    const uint32_t system_base = uint32_t(m_portal_systems.size());
    bool any = false;
    for (const render::ParticleDraw& d : main->draws) {
        if (d.distance >= k_carried_particles_m) {
            continue;
        }
        render::ParticleDraw carried = d;
        carried.first = uint32_t(m_portal_particles.size());
        for (uint32_t i = d.first; i < d.first + d.count; ++i) {
            render::GpuParticle q = main->particles[i];
            q.position = glm::vec4(to_far.point(glm::vec3(q.position)), q.position.w);
            uint32_t system;
            std::memcpy(&system, &q.size_system.y, 4);
            system += system_base;
            std::memcpy(&q.size_system.y, &system, 4);
            m_portal_particles.push_back(q);
        }
        m_portal_particle_draws.push_back(carried);
        any = true;
    }
    if (!any) {
        return own;
    }
    m_portal_systems.insert(m_portal_systems.end(), main->systems.begin(), main->systems.end());
    // Back to front: the carry keeps distances, the far camera being the near one carried.
    std::ranges::stable_sort(m_portal_particle_draws, std::greater<>{}, &render::ParticleDraw::distance);
    m_portal_particle_frame = {m_portal_particles, m_portal_systems, m_portal_particle_draws};
    return &m_portal_particle_frame;
}

void App::prepare_portal(const render::View& view, float time) {
    if (m_portal_door < 0) {
        return;
    }
    const DoorPortal& p = m_doors[size_t(m_portal_door)];
    Space& far = p.same_space ? *m_space : *other_space(m_space->world().doors[p.index].destination_space, false)->space;
    render::Environment& env = far.environment();
    const float dt = std::max(time - m_portal_time, 0.0f);
    m_portal_time = time;
    m_portal_input.view = &m_portal_view;
    m_portal_input.prepass = {};
    // What the main view's frame gives its space, for this one.
    if (m_portal_water) {
        m_portal_water->update(far.world(), m_portal_view, env, m_sky ? m_sky->water_multiplier() : glm::vec3(1.0f),
                               m_sky ? m_sky->sun_glare() : 0.0f, 11, time, m_sky ? m_sky->hour() * 3600.0f : 0.0f, dt,
                               m_portal_view.position, false);
    }
    if (m_portal_fx) {
        m_portal_fx->update(far.world(), m_portal_view, dt);
    }
    if (p.same_space) {
        m_portal_input.space = far.portal_space_view(m_portal_view, m_space_settings);
    } else {
        if (m_portal_water) {
            far.set_rain_ripples(m_portal_water->ripple_places(), m_sky ? m_sky->rain_ripples() : 0.0f);
        }
        if (m_portal_fx) {
            m_portal_fx_shapes.clear();
            for (const WorldFx::Shape& s : m_portal_fx->shapes()) {
                m_portal_fx_shapes.push_back({s.model, s.submesh, s.rotation, s.position, s.base_color, s.uv, s.animated});
            }
            // What the player holds or casts goes through the doorway with it (carried_particles).
            if (m_walk && m_world_fx) {
                const PortalTransform& t = p.to_far;
                for (const WorldFx::Shape& s : m_world_fx->shapes()) {
                    if (glm::distance(s.position, view.position) < k_carried_particles_m) {
                        const glm::mat3 r(t.vector(s.rotation[0]), t.vector(s.rotation[1]), t.vector(s.rotation[2]));
                        m_portal_fx_shapes.push_back({s.model, s.submesh, r, t.point(s.position), s.base_color, s.uv, s.animated});
                    }
                }
            }
            far.set_fx_shapes(m_portal_fx_shapes);
        }
        far.build_draws(m_portal_view, m_space_settings);
        m_portal_input.space = far.space_view();
    }
    // The occlusion map is one target: an exterior's own view has it first.
    if (env.sky.exterior && m_precip_occlusion && !m_space->environment().sky.exterior) {
        m_portal_input.prepass = m_precip_occlusion->frame(far, m_render_settings, m_portal_view, env.sky);
    }
    m_portal_input.space.environment = env;
    const glm::vec3 shadow_dir = m_portal_shadow_direction.update(env.sun_dir, m_portal_view.position, dt,
                                                                  m_render_settings.shadows.static_update_scale);
    m_portal_sun = far.interior() || (far.world().flags & osw::world_no_sky)
                       ? render::SunShadows{}
                       : render::compute_sun_shadows(m_portal_view, shadow_dir, env.near_units, env.far_units,
                                                     m_render_settings.shadows, false);
    hold_far_cascade(m_portal_sun, m_portal_view, shadow_dir, &far);
    // A door within one space builds its lists beside the main frame's, which the frame still needs.
    if (p.same_space) {
        far.begin_portal_shadows();
    }
    far.build_shadow_draws(m_portal_view, m_portal_sun, m_render_settings.shadows, m_space_settings);
    const float fov_h = 2.0f * std::atan(std::tan(view.fov_y * 0.5f) * view.aspect);
    far.build_shadow_lights(m_portal_view, m_portal_sun, m_render_settings.shadows, glm::degrees(fov_h) / 80.0f,
                            far.interior(), m_space_settings);
    render::SpaceView& sv = m_portal_input.space;
    if (p.same_space) {
        far.end_portal_shadows();
        sv.lights = far.portal_point_lights();
    }
    sv.particles = p.same_space || !m_walk ? (m_portal_fx ? m_portal_fx->frame() : nullptr) : carried_particles(p.to_far);
    sv.water = m_portal_water ? m_portal_water->frame() : nullptr;
    sv.sun_shadows = &m_portal_sun;
    for (uint32_t c = 0; c < render::k_max_cascades; ++c) {
        sv.shadow_draws[c] = p.same_space ? &far.portal_shadow_draws(c) : &far.shadow_draws(c);
    }
    sv.shadow_lights = p.same_space ? far.portal_shadow_lights() : far.shadow_lights();
    for (uint32_t i = 0; i < render::k_max_shadow_lights; ++i) {
        sv.shadow_light_draws[i] = p.same_space ? &far.portal_shadow_light_draws(i) : &far.shadow_light_draws(i);
    }
    for (int k = 0; k < 4; ++k) {
        m_portal_input.quad[k] = p.near.corner(k);
    }
}

void App::preload_exteriors() {
    if (!m_space || !m_space->interior() || !m_doors_preload) {
        return;
    }
    // Making an exterior world's terrain tables takes tens of milliseconds, so a load that starts in an interior
    // makes those of the worlds its doors open onto now, not when a door comes near.
    for (const auto& d : m_space->world().doors) {
        if (!(d.flags & osw::door_to_interior) && !(d.flags & osw::door_same_space)) {
            if (OtherSpace* o = other_space(d.destination_space, true); !o->space && o->loading.valid()) {
                o->loading.wait();
            }
        }
    }
    finish_space_loads();
}

std::string App::current_location() const {
    if (!m_space) {
        return {};
    }
    const auto& w = m_space->world();
    if (!w.cell_regions.empty()) {
        const float cs = w.cell_size;
        const int32_t cx = int32_t(std::floor(m_camera.position.x / cs));
        const int32_t cy = int32_t(std::floor(m_camera.position.y / cs));
        auto it = std::lower_bound(w.cells.begin(), w.cells.end(), std::pair(cy, cx), [](const osw::Cell& c, auto k) {
            return std::pair(c.y, c.x) < k;
        });
        if (it != w.cells.end() && it->x == cx && it->y == cy) {
            const uint16_t r = w.cell_regions[size_t(it - w.cells.begin())];
            if (r > 0 && r <= w.regions.size()) {
                return w.regions[r - 1].edid;
            }
        }
    }
    return w.edid;
}

std::string App::doors_json() const {
    std::string j = "[";
    if (m_space) {
        const auto& doors = m_space->world().doors;
        for (size_t i = 0; i < m_doors.size(); ++i) {
            const DoorPortal& p = m_doors[i];
            const osw::Door& d = doors[p.index];
            j += std::format("{}{{\"door\": \"{:08X}\", \"to\": \"{:08X}\", \"space\": \"{}\", \"distance\": {:.2f}, "
                             "\"open\": {}, \"seamless\": {}, \"same_space\": {}, \"gate\": {}, \"centre\": [{:.3f}, {:.3f}, "
                             "{:.3f}], \"normal\": [{:.3f}, {:.3f}], \"size\": [{:.3f}, {:.3f}], \"far_centre\": [{:.3f}, {:.3f}, "
                             "{:.3f}], \"far_normal\": [{:.3f}, {:.3f}], \"far_size\": [{:.3f}, {:.3f}], "
                             "\"camera_carried\": [{:.3f}, {:.3f}, {:.3f}], \"to_far\": {{\"from\": [{:.5f}, {:.5f}, {:.5f}], "
                             "\"to\": [{:.5f}, {:.5f}, {:.5f}], \"angle\": {:.7f}, \"floored\": {}}}}}",
                             i ? ", " : "", d.form_id, d.destination, d.destination_space,
                             glm::length(m_camera.position - p.near.centre), p.open ? "true" : "false",
                             p.seamless ? "true" : "false", p.same_space ? "true" : "false",
                             p.gate ? "true" : "false", p.near.centre.x,
                             p.near.centre.y, p.near.centre.z, p.near.normal.x, p.near.normal.y,
                             2.0f * p.near.half_width, 2.0f * p.near.half_height, p.far.centre.x, p.far.centre.y,
                             p.far.centre.z, p.far.normal.x, p.far.normal.y, 2.0f * p.far.half_width,
                             2.0f * p.far.half_height, p.to_far.point(m_camera.position).x,
                             p.to_far.point(m_camera.position).y, p.to_far.point(m_camera.position).z, p.to_far.from.x,
                             p.to_far.from.y, p.to_far.from.z, p.to_far.to.x, p.to_far.to.y, p.to_far.to.z, p.to_far.angle,
                             p.floored ? "true" : "false");
        }
    }
    return j + "]";
}

std::string App::door_command(const std::vector<std::string>& args) {
    if (args.empty()) {
        throw Error("door: list | open|go <formid|nearest> | through | close <formid|nearest|all> | auto on|off");
    }
    const std::string& sub = args[0];
    if (sub == "list") {
        return doors_json();
    }
    if (sub == "auto") {
        m_doors_auto = args.size() < 2 || args[1] != "off";
        return m_doors_auto ? "on" : "off";
    }
    if (sub == "rooms") {
        // The doors between rooms here, nearest first.
        std::string out = "[";
        std::vector<std::pair<float, uint32_t>> order;
        const auto& refs = m_space->world().refs;
        for (uint32_t i : m_room_doors) {
            const glm::vec3 p(refs[i].position[0], refs[i].position[1], refs[i].position[2]);
            order.push_back({glm::length(p - m_camera.position), i});
        }
        std::sort(order.begin(), order.end());
        for (size_t k = 0; k < order.size() && k < 12; ++k) {
            const osw::Ref& r = refs[order[k].second];
            out += std::format("{}{{\"ref\": \"{:08X}\", \"base\": \"{:08X}\", \"distance\": {:.1f}, \"units\": [{:.1f}, {:.1f}, {:.1f}], \"heading\": {:.1f}}}",
                               k ? ", " : "", r.form_id, r.base_form_id, order[k].first,
                               r.position[0] / units::k_skyrim_unit_m, r.position[1] / units::k_skyrim_unit_m,
                               r.position[2] / units::k_skyrim_unit_m, glm::degrees(r.source_euler[2]));
        }
        return out + "]";
    }
    if (sub == "activate") {
        activate_door(); // what the Activate key does: the door the camera points at, within reach
        return "activated";
    }
    if (sub == "floors") {
        // The doors in reach: where the floor stands on each side, against the doorway's bottom.
        std::string out = "[";
        const auto field = [](std::optional<float> v) { return v ? std::format("{:.3f}", *v) : std::string("null"); };
        for (const DoorPortal& p : m_doors) {
            if (glm::length(m_camera.position - p.near.centre) > 12.0f || !p.seamless) {
                continue;
            }
            const osw::Door& d = m_space->world().doors[p.index];
            OtherSpace* o = p.same_space ? nullptr : other_space(d.destination_space, false);
            physics::World* far = p.same_space ? m_physics.get() : o ? o->physics.get() : nullptr;
            out += std::format("{}{{\"door\": \"{:08X}\", \"space\": \"{}\", \"near\": {}, \"far\": {}}}", out.size() > 1 ? ", " : "",
                               d.form_id, d.destination_space, field(floor_offset(m_physics.get(), p.near)),
                               field(floor_offset(far, p.far)));
        }
        return out + "]";
    }
    if (sub == "probe") {
        // What collides along the view, nearest first: what stops a walker at a doorway.
        if (!m_physics) {
            throw Error("door probe: no collision here (mode walk)");
        }
        const float reach = args.size() > 1 ? std::stof(args[1]) : 3.0f;
        std::string out = "[";
        for (const auto& h : m_physics->sphere_sweep(m_camera.position, m_camera.position + m_camera.forward() * reach, 0.05f, ~0ull)) {
            const auto& refs = m_space->world().refs;
            const auto r = std::find_if(refs.begin(), refs.end(), [&](const osw::Ref& x) { return x.form_id == h.ref; });
            const bool known = r != refs.end();
            out += std::format("{}{{\"ref\": \"{:08X}\", \"base\": \"{:08X}\", \"mesh\": \"{}\", \"distance\": {:.3f}, \"layer\": {}}}",
                               out.size() > 1 ? ", " : "", h.ref, known ? r->base_form_id : 0u,
                               known && r->mesh < m_space->world().meshes.size() ? m_space->world().meshes[r->mesh] : "",
                               h.fraction * reach, h.layer);
        }
        return out + "]";
    }
    if (sub == "through") {
        // Steps the free camera through the doorway `go` stood it at: the next frame crosses.
        m_camera.position -= m_camera.forward() * -2.0f;
        return "stepped";
    }
    if (sub != "open" && sub != "close" && sub != "go") {
        throw Error("door: unknown subcommand " + sub);
    }
    const bool open = sub != "close";
    const std::string which = args.size() > 1 ? args[1] : "nearest";
    std::vector<DoorPortal*> targets;
    if (which == "all") {
        for (auto& p : m_doors) {
            targets.push_back(&p);
        }
    } else if (which == "nearest") {
        DoorPortal* best = nullptr;
        float best_d = 1e30f;
        for (auto& p : m_doors) {
            const float dist = glm::length(m_camera.position - p.near.centre);
            if (dist < best_d) {
                best = &p;
                best_d = dist;
            }
        }
        if (best) {
            targets.push_back(best);
        }
    } else {
        const uint32_t form = static_cast<uint32_t>(std::stoul(which, nullptr, 16));
        for (auto& p : m_doors) {
            if (m_space->world().doors[p.index].form_id == form) {
                targets.push_back(&p);
            }
        }
    }
    if (targets.empty()) {
        if (which == "nearest" || which == "all") {
            return "no doors";
        }
        throw Error("door: no such door here: " + which);
    }
    std::string result;
    for (DoorPortal* p : targets) {
        if (open && !p->same_space && p->seamless) {
            // A script waits for the far space instead of retrying.
            OtherSpace* o = other_space(m_space->world().doors[p->index].destination_space, true);
            if (!o->space && o->loading.valid()) {
                o->loading.wait();
                finish_space_loads();
            }
        }
        const bool ok = set_door_open(*p, open, true);
        if (sub == "go") {
            // A metre in front of it, looking in.
            m_camera.position = p->near.centre + p->near.normal * 1.0f;
            m_camera.yaw = std::atan2(-p->near.normal.x, -p->near.normal.y);
            m_camera.set_pitch(0.0f);
            m_door_last_camera = m_camera.position;
        }
        result += std::format("{}{:08X} {}", result.empty() ? "" : ", ", m_space->world().doors[p->index].form_id,
                              ok ? (open ? "open" : "closed") : "unchanged");
    }
    return result;
}

} // namespace spud::engine
