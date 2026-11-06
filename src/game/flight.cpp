// Fliers: take off, fly a spline, orbit, hover and land.

#include "core/game_random.hpp"
#include "game/ai.hpp"
#include "game/game.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spud::game {

namespace {

constexpr float k_pi = std::numbers::pi_v<float>;

float flat(const NavPoint& a, const NavPoint& b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }

NavPoint lerp(const NavPoint& a, const NavPoint& b, float t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

// Catmull-Rom through the points, at `steps` samples a span.
std::vector<NavPoint> spline(const std::vector<NavPoint>& p, int steps) {
    std::vector<NavPoint> out;
    if (p.size() < 2) {
        return p;
    }
    for (size_t i = 0; i + 1 < p.size(); ++i) {
        const NavPoint& p0 = p[i ? i - 1 : 0];
        const NavPoint& p1 = p[i];
        const NavPoint& p2 = p[i + 1];
        const NavPoint& p3 = p[std::min(i + 2, p.size() - 1)];
        for (int s = 0; s < steps; ++s) {
            const float t = float(s) / float(steps), t2 = t * t, t3 = t2 * t;
            NavPoint q{};
            for (int k = 0; k < 3; ++k) {
                q[k] = 0.5f * (2.0f * p1[k] + (p2[k] - p0[k]) * t + (2.0f * p0[k] - 5.0f * p1[k] + 4.0f * p2[k] - p3[k]) * t2 +
                               (3.0f * p1[k] - p0[k] - 3.0f * p2[k] + p3[k]) * t3);
            }
            out.push_back(q);
        }
    }
    out.push_back(p.back());
    return out;
}

} // namespace

float Ai::floor_under(const NavPoint& p, float radius) const {
    const float ground = ground_height ? ground_height(p[0], p[1]) : std::numeric_limits<float>::quiet_NaN();
    return std::isfinite(ground) ? std::max(p[2], ground + radius) : p[2];
}

// the goal raised over the ground, then steps from the actor's course toward it, each turning
// at most the step's share of 90 degrees, until the goal is within 4R and within the turn.
std::vector<NavPoint> Ai::fly_path(const Agent& a, const NavPoint& goal_in) const {
    const float r = a.flight_radius;
    NavPoint goal = goal_in;
    goal[2] = floor_under(goal, r);
    std::vector<NavPoint> out{a.position};
    float heading = a.heading;
    NavPoint at{a.position[0] + std::sin(heading) * 2.0f * r, a.position[1] + std::cos(heading) * 2.0f * r, a.position[2]};
    out.push_back(at); // the curve leaves on the flight line
    const float step = 2.0f * r;
    const float turn = k_pi / 2.0f;
    for (int i = 0; i < 64; ++i) {
        const float want = std::atan2(goal[0] - at[0], goal[1] - at[1]);
        const float off = std::remainder(want - heading, 2.0f * k_pi);
        if (flat(at, goal) <= 4.0f * r && std::abs(off) <= turn) {
            break;
        }
        heading += std::clamp(off, -turn, turn);
        const float left = flat(at, goal);
        const float t = left > 0 ? std::min(step / left, 1.0f) : 1.0f;
        at = {at[0] + std::sin(heading) * step, at[1] + std::cos(heading) * step, at[2] + (goal[2] - at[2]) * t};
        at[2] = floor_under(at, r);
        out.push_back(at);
    }
    out.push_back(goal);
    return out;
}

void Ai::fly_along(Agent& a, const std::vector<NavPoint>& points) {
    Flight& f = a.flight;
    f.line = spline(points, 8);
    f.at = 0;
    f.along = 0;
}

// The ring around the centre (PathingRequestFlyOrbit): radius / R points, at least 3, each over the ground and none below
// the ring's mean height; entered at the nearest point at least 3R from the actor.
void Ai::start_orbit(Agent& a, const NavPoint& centre, float min_radius, float max_radius, float height) {
    GameRandom& random = GameRandom::shared();
    const float r = a.flight_radius;
    const float radius = min_radius + (max_radius - min_radius) * float(random.next_unit());
    const int count = std::max(int(std::ceil(radius / r)), 3);
    std::vector<NavPoint> ring;
    float mean = 0;
    for (int i = 0; i < count; ++i) {
        const float angle = 2.0f * k_pi * float(i) / float(count);
        NavPoint p{centre[0] + std::sin(angle) * radius, centre[1] + std::cos(angle) * radius, centre[2] + height};
        p[2] = floor_under(p, r);
        mean += p[2] / float(count);
        ring.push_back(p);
    }
    size_t entry = 0;
    float best = std::numeric_limits<float>::max();
    for (size_t i = 0; i < ring.size(); ++i) {
        ring[i][2] = std::max(ring[i][2], mean);
        const float d = flat(ring[i], a.position);
        if (d >= 3.0f * r && d < best) {
            best = d;
            entry = i;
        }
    }
    // Round the way the actor is already going.
    const NavPoint& next = ring[(entry + 1) % ring.size()];
    const float along = std::sin(a.heading) * (next[0] - ring[entry][0]) + std::cos(a.heading) * (next[1] - ring[entry][1]);
    const int direction = along >= 0 ? 1 : -1;
    std::vector<NavPoint> points = fly_path(a, ring[entry]);
    for (size_t i = 1; i <= ring.size(); ++i) {
        points.push_back(ring[(entry + size_t(int(ring.size()) + direction * int(i))) % ring.size()]);
    }
    fly_along(a, points);
}

const AiLanding* Ai::landing_near(const Agent& a) const {
    if (a.landing_marker) {
        return &a.forced_landing;
    }
    const auto it = m_landings.find(a.space);
    if (it == m_landings.end()) {
        return nullptr;
    }
    const AiLanding* best = nullptr;
    float distance = std::numeric_limits<float>::max();
    for (const AiLanding& l : it->second) {
        const float d = flat(l.position, a.position);
        if (!l.crash && d < distance) {
            bool clear = true;
            const float room = m_game.settings().number("fDragonLandingZoneClearRadius", 256.0f);
            for (const uint32_t ref : m_high) {
                const Agent* o = agent(ref);
                clear = clear && (!o || o->ref == a.ref || flat(o->position, l.position) > room);
            }
            if (clear) {
                best = &l;
                distance = d;
            }
        }
    }
    return best;
}

void Ai::order_flight(uint32_t ref, AiMode mode, const NavPoint& centre) {
    if (const auto it = m_agents.find(ref); it != m_agents.end()) {
        Agent& a = it->second;
        a.flight.order = mode;
        a.flight.centre = centre;
        a.flight.line.clear();
        a.flight.leaving = false;
    }
}

void Ai::flight_event(uint32_t ref, std::string_view tag) {
    const auto it = m_agents.find(ref);
    if (it == m_agents.end()) {
        return;
    }
    Agent& a = it->second;
    // The Flight* handlers set the state the graph has reached.
    if (tag == "FlightTakeOff") {
        a.fly = FlyState::take_off;
    } else if (tag == "FlightCruising") {
        a.fly = FlyState::cruising;
    } else if (tag == "FlightHovering") {
        a.fly = FlyState::hovering;
    } else if (tag == "FlightLanding") {
        a.fly = FlyState::landing;
    } else if (tag == "FlightPerching") {
        a.fly = FlyState::perching;
    } else if (tag == "FlightLand" || tag == "FlightLandEnd") {
        a.fly = FlyState::none;
        a.spot = {};
        a.path.clear();
        a.next = 0;
    }
}

bool Ai::wants_air(const Agent& a) const {
    if (!a.allow_flying) {
        return false;
    }
    if (a.flight.order != AiMode::stay) {
        return a.flight.order == AiMode::orbit || a.flight.order == AiMode::hover;
    }
    return a.package && (a.package->mode == AiMode::orbit || a.package->mode == AiMode::hover);
}

// the Fly and Orbit procedures and landing, one step.
void Ai::fly(Agent& a, float dt) {
    Flight& f = a.flight;
    const bool hover = f.order == AiMode::hover || (f.order == AiMode::stay && a.package && a.package->mode == AiMode::hover);
    const bool air = wants_air(a);
    f.timer -= dt;
    switch (a.fly) {
    case FlyState::none:
    case FlyState::perching:
        if (air) {
            a.idle = a.fly == FlyState::perching ? "FlyStartPerch" : "FlyStartTakeOff";
            a.fly = FlyState::take_off;
            f.timer = 8.0f; // until the graph says FlightCruising
        }
        return;
    case FlyState::take_off:
        if (f.timer <= 0.0f) {
            a.fly = FlyState::cruising;
        }
        return;
    case FlyState::landing:
        if (f.timer <= 0.0f) {
            a.fly = FlyState::none;
            a.position = f.goal;
        }
        return;
    case FlyState::hovering:
        if (!air || !hover) {
            a.idle = "HoverStop";
            a.fly = FlyState::cruising;
            f.line.clear();
        }
        return;
    default:
        break;
    }
    const float speed = a.package && a.package->preferred_speed >= 2 ? m_fly_run_speed : m_fly_walk_speed;
    a.speed = speed;
    if (f.at + 1 >= f.line.size()) {
        if (f.leaving) { // over the landing spot
            a.idle = "FlyStopDefault";
            a.fly = FlyState::landing;
            f.timer = 10.0f;
            return;
        }
        const AiPackage* p = a.package;
        const bool ordered = f.order != AiMode::stay;
        const NavPoint centre = ordered ? f.centre : a.location.point;
        if (!air) {
            const AiLanding* l = landing_near(a);
            f.goal = l ? l->position : NavPoint{a.position[0], a.position[1], floor_under(a.position, 0.0f)};
            NavPoint over = f.goal;
            over[2] += a.flight_radius;
            f.leaving = true;
            fly_along(a, fly_path(a, over));
        } else if (hover) {
            NavPoint over = centre;
            over[2] += p && !ordered && p->floats.size() > 0 ? p->floats[0] : 1000.0f;
            if (flat(over, a.position) < 4.0f * a.flight_radius) {
                a.idle = "HoverStart";
                a.fly = FlyState::hovering;
                return;
            }
            fly_along(a, fly_path(a, over));
        } else {
            const auto value = [&](size_t i, float fallback) { return p && !ordered && p->floats.size() > i ? p->floats[i] : fallback; };
            start_orbit(a, centre, value(0, 3000.0f), value(1, 3500.0f), value(2, 1000.0f));
        }
        if (f.line.size() < 2) {
            return;
        }
    }
    // The follower: distance along the curve, heading and pitch from its tangent, no roll.
    float left = speed * dt;
    while (f.at + 1 < f.line.size()) {
        const NavPoint& from = f.line[f.at];
        const NavPoint& to = f.line[f.at + 1];
        const float length = std::sqrt((to[0] - from[0]) * (to[0] - from[0]) + (to[1] - from[1]) * (to[1] - from[1]) +
                                       (to[2] - from[2]) * (to[2] - from[2]));
        if (f.along + left < length) {
            f.along += left;
            a.position = lerp(from, to, length > 0 ? f.along / length : 1.0f);
            a.heading = std::atan2(to[0] - from[0], to[1] - from[1]);
            a.pitch = -std::atan2(to[2] - from[2], flat(from, to));
            return;
        }
        left -= length - f.along;
        f.along = 0;
        ++f.at;
    }
    a.position = f.line.back();
}

} // namespace spud::game
