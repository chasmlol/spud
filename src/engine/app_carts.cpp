// Carts: the tether that pulls one and the riders it carries.
#include "engine/app_internal.hpp"
#include "core/units.hpp"
#include "game/game.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spud::engine {

namespace {

// fHorseOffsetY:Cart, in the image: where the tether puts the horse, ahead of the cart.
constexpr float k_horse_offset = 200.0f;
// The real cart's front axle stays this far behind the horse as it pulls, on average (the ride, measured), and its
// back axle this far behind the front one (WheelBackLeft in the model, which the measured turning also gives).
constexpr float k_pull = 215.0f, k_axles = 215.0f, k_track = 146.0f;
// How the seat faces from the cart's nose, before SetSittingRotation: the ride's view, measured, less MQ101's -55.
constexpr float k_seat_facing = 85.0f * std::numbers::pi_v<float> / 180.0f;
// MQ101 sets SittingAngleLimit to this for the ride.
constexpr float k_look_limit = 2.618f;
constexpr float k_settle_s = 0.15f; // what the wheels and the body's weight smooth out of the road (*inferred*)

float wrapped(float radians) { return std::remainder(radians, 2.0f * std::numbers::pi_v<float>); }

// The body on its wheels: tipped about its own axes, then turned. A placement's angles go the other way round.
glm::quat turned(float pitch, float roll, float heading) {
    return glm::angleAxis(-heading, glm::vec3(0, 0, 1)) * glm::angleAxis(-pitch, glm::vec3(1, 0, 0)) *
           glm::angleAxis(-roll, glm::vec3(0, 1, 0));
}

} // namespace

const App::CartTether* App::cart_of(uint32_t rider) const {
    const auto vehicle = m_vehicles.find(rider);
    if (vehicle == m_vehicles.end()) {
        return nullptr;
    }
    const auto cart = std::ranges::find(m_carts, vehicle->second, &CartTether::cart);
    return cart == m_carts.end() ? nullptr : &*cart;
}

void App::tether_cart(uint32_t cart, uint32_t horse) {
    const auto at = m_game->ref_position(cart);
    const game::Agent* a = m_game->ai().agent(horse);
    if (!at || !a) {
        return;
    }
    std::erase_if(m_carts, [cart](const CartTether& t) { return t.cart == cart; });
    const std::array<float, 3> angle = m_game->ref_angle(cart);
    const float heading = angle[2], s = std::sin(heading), c = std::cos(heading);
    const std::array<float, 3> place{(*at)[0] + k_horse_offset * s, (*at)[1] + k_horse_offset * c, a->position[2]};
    m_game->move_ref(horse, place);
    m_game->changes(horse).angle = angle;
    m_ref_moved(horse);
    m_game->ai().place_at(horse, a->space, game::NavPoint{place[0], place[1], place[2]}, heading);
    CartTether t;
    t.cart = cart;
    t.horse = horse;
    t.front = {(*at)[0], (*at)[1]};
    t.rear = t.front - glm::vec2(s, c) * k_axles;
    t.z = (*at)[2];
    t.pitch = angle[0];
    t.roll = angle[1];
    t.heading = heading;
    t.trail = {glm::vec3(t.rear, t.z), glm::vec3(t.front, t.z)};
    m_carts.push_back(t);
}

void App::hold_cart(uint32_t cart, bool held) {
    if (const auto t = std::ranges::find(m_carts, cart, &CartTether::cart); t != m_carts.end()) {
        t->held = held;
    }
}

void App::set_vehicle(uint32_t rider, uint32_t vehicle) {
    if (!vehicle || vehicle == rider) {
        m_vehicles.erase(rider);
        m_seat_idles.erase(rider);
        if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
            actors->set_tilt(rider, std::nullopt);
        }
        m_game->ai().hold(rider, false);
        if (rider == game::k_player_ref) {
            m_game->set_player_sit_state(0);
        }
        return;
    }
    m_vehicles[rider] = vehicle;
    m_game->ai().hold(rider, true); // it goes where the cart goes, whatever its package would walk it to
    if (rider == game::k_player_ref) {
        m_game->set_player_sit_state(3); // sitting, as long as it rides
        m_vehicle_base.reset();
    }
}

// ExitCartEnd: the clip has walked it off the cart, and it stands where its body is now.
void App::leave_vehicle(uint32_t rider) {
    const CartTether* t = cart_of(rider);
    ActorSet* actors = m_space ? m_space->actors() : nullptr;
    if (t && actors) {
        const bool player = rider == game::k_player_ref;
        const glm::vec3 body = actors->node_offset(player ? ActorSet::k_player_body : rider, "npc pelvis [pelv]");
        const glm::vec3 at = glm::vec3(t->front, t->z) * units::k_skyrim_unit_m +
                             turned(0.0f, 0.0f, t->heading) * glm::vec3(body.x, body.y, 0.0f);
        if (player) {
            if (m_player) {
                m_player->teleport(at);
            }
        } else if (const game::Agent* a = m_game->ai().agent(rider)) {
            const glm::vec3 u = at / units::k_skyrim_unit_m;
            m_game->ai().place_at(rider, a->space, game::NavPoint{u.x, u.y, u.z}, t->heading);
        }
    }
    set_vehicle(rider, 0);
}

void App::update_carts(float dt) {
    if (!m_game || m_carts.empty()) {
        return;
    }
    const float settle = 1.0f - std::exp(-dt / k_settle_s);
    const auto land = [this](glm::vec2 at, float otherwise) {
        const auto height = terrain_height(at.x * units::k_skyrim_unit_m, at.y * units::k_skyrim_unit_m);
        return height ? *height / units::k_skyrim_unit_m : otherwise;
    };
    // Its wheels go where the horse went, over bridges and all, and the road's height is what it walked on.
    const auto trail = [](const CartTether& t, glm::vec2 at) {
        float nearest = 1e30f, z = t.z;
        for (const glm::vec3& p : t.trail) {
            if (const float d = glm::distance(glm::vec2(p), at); d < nearest) {
                nearest = d;
                z = p.z;
            }
        }
        return z;
    };
    for (CartTether& t : m_carts) {
        const game::Agent* horse = m_game->ai().agent(t.horse);
        if (t.held || !horse) {
            continue;
        }
        const glm::vec3 walked(horse->position[0], horse->position[1], horse->position[2]);
        if (t.trail.empty() || glm::distance(glm::vec2(t.trail.back()), glm::vec2(walked)) > 16.0f) {
            t.trail.push_back(walked);
            if (t.trail.size() > 64) {
                t.trail.pop_front();
            }
        }
        const glm::vec2 to_horse = glm::vec2(walked) - t.front;
        const float pulled = glm::length(to_horse);
        if (pulled <= k_pull) {
            continue;
        }
        t.front += to_horse / pulled * (pulled - k_pull);
        const glm::vec2 along = t.front - t.rear;
        const float length = glm::length(along);
        const glm::vec2 ahead = length > 0.001f ? along / length : glm::vec2(std::sin(t.heading), std::cos(t.heading));
        t.rear = t.front - ahead * k_axles;
        t.heading = std::atan2(ahead.x, ahead.y);
        const glm::vec2 side(ahead.y, -ahead.x), middle = (t.front + t.rear) * 0.5f;
        const float front = trail(t, t.front), rear = trail(t, t.rear);
        const float left = land(middle - side * (k_track * 0.5f), 0.0f);
        const float right = land(middle + side * (k_track * 0.5f), 0.0f);
        t.z += (front - t.z) * settle;
        t.pitch += (std::atan2(rear - front, k_axles) - t.pitch) * settle;
        t.roll += (std::atan2(right - left, k_track) - t.roll) * settle;
        m_game->move_ref(t.cart, {t.front.x, t.front.y, t.z});
        m_game->changes(t.cart).angle = std::array<float, 3>{0.0f, 0.0f, t.heading};
        if (m_space) {
            m_space->move_ref(t.cart, glm::vec3(t.front, t.z) * units::k_skyrim_unit_m, turned(t.pitch, t.roll, t.heading));
        }
    }
    if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
        std::erase_if(m_vehicle_idles, [actors](const auto& idle) {
            return actors->play_idle(idle.first == game::k_player_ref ? ActorSet::k_player_body : idle.first, idle.second);
        });
    }
    ActorSet* actors = m_space ? m_space->actors() : nullptr;
    for (const auto& [rider, vehicle] : m_vehicles) {
        const CartTether* t = cart_of(rider);
        if (!t) {
            continue;
        }
        // A seat tips with the cart: the clip puts the body in it, about the cart's own point.
        if (actors) {
            actors->set_tilt(rider, glm::vec2(t->pitch, t->roll));
        }
        if (rider == game::k_player_ref) {
            if (m_player) {
                m_player->teleport(glm::vec3(t->front, t->z) * units::k_skyrim_unit_m);
            }
        } else if (const game::Agent* a = m_game->ai().agent(rider)) {
            m_game->ai().place_at(rider, a->space, game::NavPoint{t->front.x, t->front.y, t->z}, t->heading);
        }
    }
}

// An actor on its horse goes where its package goes, and the horse under it; the saddle is the offset it's shown at.
glm::vec3 App::npc_rides(const game::Agent& rider, ActorSet& actors, const glm::vec3& feet) {
    game::Ai& ai = m_game->ai();
    const auto mounted = m_npc_mounts.find(rider.ref);
    if (!rider.rides) {
        if (mounted != m_npc_mounts.end()) {
            actors.send_event(rider.ref, "HorseExit");
            actors.send_event(mounted->second, "pa_HorseExit");
            ai.hold(mounted->second, false);
            m_npc_mounts.erase(mounted);
        }
        return glm::vec3(0.0f);
    }
    if (!ai.agent(rider.rides)) {
        return glm::vec3(0.0f);
    }
    if (mounted == m_npc_mounts.end() && actors.send_event(rider.ref, "HorseEnter")) {
        actors.send_event(rider.rides, "pa_HorseEnter");
        actors.set_variable(rider.ref, "bWantMountedWeaponAnims", 1.0f);
        actors.set_variable(rider.ref, "iWantMountedWeaponAnims", 1.0f);
        ai.hold(rider.rides, true);
        m_npc_mounts[rider.ref] = rider.rides;
    }
    ai.place_at(rider.rides, rider.space, rider.position, rider.heading);
    actors.move(rider.rides, feet, rider.heading, rider.next < rider.path.size() ? rider.speed : 0.0f, 0.0f);
    const glm::vec3 saddle = actors.node_offset(rider.rides, "saddlebone");
    const float c = std::cos(rider.heading), s = std::sin(rider.heading);
    return glm::vec3(saddle.x * c + saddle.y * s, -saddle.x * s + saddle.y * c, saddle.z);
}

// Before the player's frame: the look turns with the cart, and stays within what a seat allows.
void App::vehicle_look() {
    const CartTether* t = cart_of(game::k_player_ref);
    if (!t || !m_player) {
        return;
    }
    const float base = t->heading + k_seat_facing + m_sitting_rotation;
    const float offset = m_vehicle_base ? wrapped(m_player->yaw() - *m_vehicle_base) : 0.0f;
    m_vehicle_base = base;
    m_player->set_facing(base + std::clamp(offset, -k_look_limit, k_look_limit), m_player->pitch());
}

// The first-person view from a seat: the head, wherever the clip and the cart's tilt put it.
void App::vehicle_view() {
    const CartTether* t = cart_of(game::k_player_ref);
    const ActorSet* actors = m_space ? m_space->actors() : nullptr;
    if (!t || !actors) {
        return;
    }
    // The real ride's camera sits this far from the head node, in the cart's axes (measured).
    constexpr glm::vec3 k_from_head = glm::vec3(-18.0f, -8.0f, 10.0f) * units::k_skyrim_unit_m;
    const glm::vec3 head = actors->node_offset(ActorSet::k_player_body, "npc head [head]") + k_from_head;
    m_camera.position = glm::vec3(t->front, t->z) * units::k_skyrim_unit_m + turned(t->pitch, t->roll, t->heading) * head;
    m_camera.set_pitch(m_camera.pitch - t->pitch);
}

} // namespace spud::engine
