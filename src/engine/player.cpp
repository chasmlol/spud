#include "engine/player.hpp"
#include "core/sampler.hpp"

#include "core/log.hpp"
#include "core/units.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace spud::engine {

namespace {

constexpr float k_half_pi = 0.5f * std::numbers::pi_v<float>;

} // namespace

MovementTuning MovementTuning::from_rules(const osr::Rules& rules) {
    MovementTuning t;
    t.settings = movement::Settings::from_rules(rules);
    const auto& p = rules.player;
    const osr::Race* race = rules.race(p.race);
    // The movement types the player's behaviour graph moves with (by name), else the race's defaults.
    auto pick = [&](const char* edid, int race_slot, osr::MovementType& out) {
        if (const auto* named = rules.movement_type(edid)) {
            out = *named;
        } else if (const auto* by_race = race ? rules.movement_type(race->movement_types[race_slot]) : nullptr) {
            out = *by_race;
        } else {
            log::warn("player: no movement type {} in the rules", edid);
        }
    };
    pick("NPC_Default_MT", 1, t.walk_run);
    pick("NPC_Sneaking_MT", 4, t.sneak);
    pick("NPC_Sprinting_MT", 5, t.sprint);
    pick("NPC_Swimming_MT", 2, t.swim);
    t.attacking = t.attacking_2h = t.power_attacking = t.walk_run;
    pick("NPC_Attacking_MT", 1, t.attacking);
    pick("NPC_Attacking2H_MT", 1, t.attacking_2h);
    pick("NPC_PowerAttacking_MT", 1, t.power_attacking);

    float scale = std::max(p.npc_height, 0.1f);
    if (race) {
        scale *= std::max(race->height[p.female ? 1 : 0], 0.1f);
    }
    t.body.scale = scale;
    // The new-game player's equipped weight in 1.7.104 (measured; spud has no inventory yet).
    t.body.equipped_weight = 57;
    t.body.inventory_weight = 96.5f;
    if (p.eye_height > 0) {
        t.eye_height = p.eye_height; // the first-person skeleton is not scaled by race height (measured)
    }
    const float u = units::k_skyrim_unit_m;
    // The controller's hull comes from the bound unscaled (measured: the game's manifold distances to the same
    // surfaces match the unscaled radius, 0.0077 m less than the race-scaled one); the body height the water
    // tests use is scaled (measured: swimming starts at 0.7 x 2 x hz x 1.03).
    const glm::vec3 bound = p.bound_half[2] > 0 ? glm::vec3(p.bound_half[0], p.bound_half[1], p.bound_half[2])
                                                : glm::vec3(23.0f, 17.5f, 64.0f) * u;
    t.controller.half_extents = bound;
    t.body_height = 2.0f * bound.z * scale;
    const float radius = 0.5f * (t.controller.half_extents.x + t.controller.half_extents.y);
    t.controller.step_up = std::max(31.0f * u, 0.75f * radius);
    t.controller.stairs_materials = rules.stairs_materials;
    if (p.camera3_height > 0) {
        t.camera3_height = p.camera3_height; // used unscaled, as the game does
    }
    // The camera's collision sphere is on layer 39 (L_CAMERA), not 36 (L_CAMERASPHERE): what it hits is that row.
    if (const osr::CollisionLayer* camera = rules.collision_layer(39)) {
        t.camera_layers = camera->collides;
    }
    return t;
}

Player::Player(physics::World& world, std::shared_ptr<const osw::World> water, const MovementTuning& tuning,
               const glm::vec3& feet)
    : m_world(&world), m_water(std::move(water)), m_tuning(tuning),
      m_character(std::make_unique<physics::Character>(world, tuning.controller, feet)) {
    m_curr = m_character->state();
    m_curr.position = feet;
    m_prev = m_curr;
    m_shown_feet = feet;
    m_sim.status.stamina = tuning.max_stamina;
    m_sim.status.health = tuning.max_health;
    m_sim.status.breath = tuning.settings.actor_swim_breath_base + tuning.settings.actor_swim_breath_mult * 50.0f;
    m_status = m_sim.status;
    m_job_tuning = m_tuning;
    m_thread = std::thread([this] { sim_main(); });
    launch();
}

Player::~Player() {
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    m_thread.join();
}

void Player::sim_main() {
    sampler::catch_terminate();
    std::unique_lock lock(m_mutex);
    for (;;) {
        m_cv.wait(lock, [&] { return m_stop || m_job_queued; });
        if (m_stop) {
            return;
        }
        m_job_queued = false;
        const PlayerInput in = m_job_input;
        const bool step = m_job_step;
        lock.unlock();
        tick(in, step);
        lock.lock();
        m_job = false;
        m_cv.notify_all();
    }
}

float Player::water_height(glm::vec2 p) const {
    if (!m_water || m_water->cell_size <= 0) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    const int cx = static_cast<int>(std::floor(p.x / m_water->cell_size));
    const int cy = static_cast<int>(std::floor(p.y / m_water->cell_size));
    const auto& cells = m_water->cells; // sorted by (y, x)
    auto it = std::lower_bound(cells.begin(), cells.end(), std::pair(cy, cx), [](const osw::Cell& c, std::pair<int, int> k) {
        return std::pair(c.y, c.x) < k;
    });
    if (it == cells.end() || it->x != cx || it->y != cy || !(it->flags & osw::cell_has_water)) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return it->water_height;
}

float Player::land_height(glm::vec2 p) const {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    if (!m_water || m_water->cell_size <= 0) {
        return nan;
    }
    const float cs = m_water->cell_size;
    const int cx = static_cast<int>(std::floor(p.x / cs));
    const int cy = static_cast<int>(std::floor(p.y / cs));
    const auto& cells = m_water->cells; // sorted by (y, x)
    auto it = std::lower_bound(cells.begin(), cells.end(), std::pair(cy, cx), [](const osw::Cell& c, std::pair<int, int> k) {
        return std::pair(c.y, c.x) < k;
    });
    if (it == cells.end() || it->x != cx || it->y != cy || it->land == osw::k_none || it->land >= m_water->lands.size()) {
        return nan;
    }
    const auto& h = m_water->lands[it->land].heights; // row 0 south, 33 x 33
    constexpr int n = osw::k_land_verts;
    const float fx = (p.x - float(cx) * cs) / cs * float(n - 1);
    const float fy = (p.y - float(cy) * cs) / cs * float(n - 1);
    const int col = std::clamp(static_cast<int>(fx), 0, n - 2), row = std::clamp(static_cast<int>(fy), 0, n - 2);
    const float tx = fx - float(col), ty = fy - float(row);
    const float h00 = h[size_t(row) * n + col], h10 = h[size_t(row) * n + col + 1];
    const float h01 = h[size_t(row + 1) * n + col], h11 = h[size_t(row + 1) * n + col + 1];
    // The drawn LAND mesh alternates its quads' diagonals in a checkerboard: even quads south-west to north-east,
    // odd ones south-east to north-west (RE: the land height query).
    if ((col + row) % 2 == 0) {
        return tx >= ty ? h00 + (h10 - h00) * tx + (h11 - h10) * ty : h00 + (h11 - h01) * tx + (h01 - h00) * ty;
    }
    return tx + ty <= 1.0f ? h00 + (h10 - h00) * tx + (h01 - h00) * ty
                           : h11 + (h01 - h11) * (1.0f - tx) + (h10 - h11) * (1.0f - ty);
}

std::optional<glm::vec3> Player::current_at(const glm::vec3& p) const {
    if (!m_water || m_water->cell_size <= 0 || m_water->currents.empty()) {
        return std::nullopt;
    }
    const int cx = static_cast<int>(std::floor(p.x / m_water->cell_size));
    const int cy = static_cast<int>(std::floor(p.y / m_water->cell_size));
    const auto& list = m_water->currents; // sorted by (y, x)
    auto it = std::lower_bound(list.begin(), list.end(), std::pair(cy, cx), [](const osw::CellCurrent& c, std::pair<int, int> k) {
        return std::pair(c.y, c.x) < k;
    });
    if (it == list.end() || it->x != cx || it->y != cy) {
        return std::nullopt;
    }
    const auto quads = std::span(m_water->current_quads).subspan(it->first_quad, it->quad_count);
    return movement::water_current(quads, p);
}

// The graph's iState values for attacks: iState_NPCAttacking 12, 2H 13, PowerAttacking 14 (measured in the game's graph).
static const osr::MovementType* attack_type(const MovementTuning& t, int state) {
    return state == 12 ? &t.attacking : state == 13 ? &t.attacking_2h : state == 14 ? &t.power_attacking : nullptr;
}

float Player::turn_speed() const {
    // The look input turns the player at the rotate-in-place run speed of the movement type in use.
    const MovementTuning& t = m_tuning;
    const osr::MovementType* attack = t.riding ? nullptr : attack_type(t, m_graph.graph_state);
    const osr::MovementType& mt = m_status.swimming ? t.swim : attack ? *attack : m_status.sprinting ? t.sprint : t.walk_run;
    return t.riding ? mt.rotate_while_moving : mt.rotate_in_place[1];
}

void Player::replace_character(const glm::vec3& feet) {
    m_character = std::make_unique<physics::Character>(*m_world, m_tuning.controller, feet);
    m_curr = m_character->state();
    m_curr.position = feet;
    m_prev = m_curr;
    m_shown_feet = feet;
    m_into = 0;
    m_waiting = true;
    m_jump_latch = m_sneak_latch = m_sprint_latch = false;
    m_sim.graph_sprint = m_sim.was_moving = false;
    m_sim.sprint_tail = 0;
    m_sim.last_speed = 0;
    m_sim.move = glm::vec2(0.0f);
    m_sim.world_move = glm::vec3(0.0f);
    m_sim.buffer = {};
    m_sim.camera_started = false;
    m_sim.status.sprinting = m_sim.status.graph_sprint = m_sim.status.sprint_request = m_sim.status.sneaking = false;
    m_status = m_sim.status;
}

void Player::mount(const Riding& r, const glm::vec3& feet, float heading) {
    wait();
    m_result.ran = false;
    m_on_foot = m_tuning;
    m_foot_stamina = m_status.stamina;
    m_body_set = false;
    MovementTuning& t = m_tuning;
    t.riding = true;
    t.walk_run = t.sneak = r.walk_run;
    t.sprint = r.sprint;
    t.swim = r.swim;
    t.fall = r.fall;
    t.body = {};
    t.max_stamina = r.max_stamina;
    t.stamina_rate = r.stamina_rate;
    t.stamina_rate_mult = 100;
    t.third_person_dampen_time = r.dampen_time;
    t.max_upward_pitch = r.max_upward_pitch;
    t.max_angle_before_turn = r.max_angle_before_turn;
    t.controller.half_extents = r.half_extents;
    const float radius = 0.5f * (r.half_extents.x + r.half_extents.y);
    t.controller.step_up = std::max(31.0f * units::k_skyrim_unit_m, 0.75f * radius);
    t.body_height = 2.0f * r.half_extents.z;
    // the camera's target is the mount, 0.75 of its bound's height.
    t.camera3_height = t.sneak_camera3_height = 0.75f * t.body_height;
    t.camera3.over_shoulder_pos_x = r.camera_offset.x;
    t.camera3.over_shoulder_add_y = r.camera_offset.y;
    t.camera3.over_shoulder_pos_z = r.camera_offset.z;
    m_sim.status.stamina = r.stamina;
    m_sim.status.riding = true;
    m_sim.status.mount_heading = heading;
    m_yaw = heading;
    replace_character(feet);
    launch();
}

void Player::carried_to(const glm::vec3& feet) {
    wait();
    m_character->set_position(feet);
    m_curr.position = m_prev.position = m_shown_feet = feet;
}

void Player::dismount(const glm::vec3& feet) {
    wait();
    m_result.ran = false;
    const float eye = m_tuning.eye_height;
    m_tuning = m_on_foot;
    m_tuning.eye_height = m_on_foot.riding ? eye : m_on_foot.eye_height;
    m_sim.status.stamina = m_foot_stamina;
    m_sim.status.riding = false;
    m_body_set = false;
    replace_character(feet);
    launch();
}

glm::vec2 Player::look_ahead() const {
    if (!m_camera_free) {
        return glm::vec2(0.0f); // the camera chases the heading (camera3): what the ticks say is all there is to show
    }
    const auto& s = m_tuning.settings;
    // The look vector as the tick sees it: what is on its way in one, and the counts that are waiting.
    const float dy = s.invert_y ? m_look_counts.y : -m_look_counts.y;
    const glm::vec2 waiting(m_look_counts.x * s.mouse_heading_sensitivity * s.mouse_heading_x_scale / float(k_tick),
                            dy * s.mouse_heading_sensitivity * s.mouse_heading_y_scale);
    const glm::vec2 look = waiting + m_launched.look;
    const float speed = m_tuning.camera3.free_rotation_speed * float(k_tick);
    return {look.x * speed, look.y * speed};
}

glm::vec3 Player::camera() const {
    if (m_view != View::third_person) {
        return eye();
    }
    if (!m_camera_free) {
        return m_shown_camera; // on the same fraction as camera_yaw(), so the body stays put in the view
    }
    // Free rotation: round the player from where the last tick left the camera, by the turn still to come, as the tick
    // will put it. That tick's own position and not a blend, or it would lag the angle the view is shown at.
    const float turn = look_ahead().x;
    const glm::vec3 pivot = eye();
    const glm::vec3 v = m_curr_camera - (pivot + m_curr.position - m_shown_feet);
    const float c = std::cos(turn), sn = std::sin(turn);
    return pivot + glm::vec3(v.x * c + v.y * sn, -v.x * sn + v.y * c, v.z);
}

float Player::camera_yaw() const {
    if (m_view != View::third_person) {
        return m_yaw;
    }
    if (m_camera_free) {
        return m_camera_yaw + look_ahead().x;
    }
    return m_prev_camera_yaw + std::remainder(m_camera_yaw - m_prev_camera_yaw, 2.0f * std::numbers::pi_v<float>) * m_shown_fraction;
}

float Player::camera_pitch() const {
    if (m_view != View::third_person) {
        return m_pitch;
    }
    return m_camera_free ? m_camera_pitch + look_ahead().y : glm::mix(m_prev_camera_pitch, m_camera_pitch, m_shown_fraction);
}

void Player::look_counts(glm::vec2 counts) {
    if (m_view == View::third_person) {
        m_look_counts += counts; // the tick decides between turning the player and circling the camera
        return;
    }
    const auto& s = m_tuning.settings;
    m_yaw += counts.x * s.mouse_heading_sensitivity * s.mouse_heading_x_scale * turn_speed();
    const float look_y = (s.invert_y ? counts.y : -counts.y) * s.mouse_heading_sensitivity * s.mouse_heading_y_scale;
    m_pitch = std::clamp(m_pitch - movement::pitch_delta(look_y, m_tuning.fov_deg, s), -k_half_pi, k_half_pi);
}

void Player::set_view(View view) {
    if (view == m_view) {
        return; // the game ignores a switch to the view it is in
    }
    wait();
    m_view = view;
    m_sim.camera_started = false; // the next tick starts the third-person camera
    if (view == View::third_person) {
        // Until it does, frames show the camera's last state: none yet, which is the world's origin, or wherever it was
        // left, however far away. From the eyes the player is seeing through, the tick then takes it to its place.
        m_prev_camera = m_curr_camera = m_shown_camera = eye();
        m_prev_camera_yaw = m_camera_yaw = m_yaw;
        m_prev_camera_pitch = m_camera_pitch = m_pitch;
    }
    m_look_counts = glm::vec2(0.0f);
}

void Player::wheel(float notches) {
    if (notches == 0) {
        return;
    }
    if (m_view == View::first_person) {
        if (notches < 0) {
            set_view(View::third_person);
        }
        return;
    }
    if (notches > 0 && m_camera_zoom <= m_tuning.camera3.min_current_zoom + 1e-6f) {
        set_view(View::first_person);
        return;
    }
    m_wheel -= notches;
}

void Player::set_facing(float yaw, float pitch) {
    m_yaw = yaw;
    m_pitch = std::clamp(pitch, -k_half_pi, k_half_pi);
}

void Player::set_graph(bool driven, glm::vec2 velocity, int state) {
    m_graph.graph_driven = driven;
    m_graph.graph_velocity = velocity;
    m_graph.graph_state = state;
}

void Player::launch() {
    PlayerInput in;
    m_launched_scripted = !m_script.empty();
    m_launched_step = m_launched_scripted || !m_hold;
    const bool third = m_view == View::third_person;
    if (m_launched_scripted) {
        in = m_script.front();
        m_script.pop_front();
        // A scripted pad changes now and the game's device reads it on the next frame (the game at
        // the start of the frame): the tick runs on the pad state queued for the tick before.
        const PlayerInput polled = m_pad_polled;
        m_pad_polled = in.gamepad ? in : PlayerInput{};
        if (polled.gamepad) {
            in.gamepad = true;
            in.run = true;
            in.move = polled.move;
            in.right_stick = polled.right_stick;
            in.jump = polled.jump;
            in.sneak = polled.sneak;
            in.sprint_press = polled.sprint_press;
        } else if (in.gamepad) {
            in.gamepad = false;
            in.move = glm::vec2(0.0f);
            in.right_stick = glm::vec2(0.0f);
            in.jump = in.sneak = in.sprint_press = false;
        }
        if (in.gamepad) {
            in.look = movement::gamepad_look(in.right_stick, static_cast<float>(k_tick), m_pad_full_time,
                                             m_tuning.settings);
        } else {
            m_pad_full_time = 0;
        }
        // Scripted look input turns the player tick by tick (the game's yaw rate and pitch per update); in third
        // person the tick decides whether it turns the player or circles the camera.
        if (!third) {
            m_yaw += in.look.x * turn_speed() * static_cast<float>(k_tick);
            m_pitch = std::clamp(m_pitch - movement::pitch_delta(in.look.y, m_tuning.fov_deg, m_tuning.settings),
                                 -k_half_pi, k_half_pi);
        }
    } else if (m_launched_step) {
        in = m_live;
        in.jump = m_jump_latch;
        in.sneak = m_sneak_latch;
        in.sprint_press = m_sprint_latch;
        m_jump_latch = m_sneak_latch = m_sprint_latch = false;
        in.zoom = m_wheel;
        m_wheel = 0;
        if (in.gamepad) {
            in.look = movement::gamepad_look(in.right_stick, static_cast<float>(k_tick), m_pad_full_time,
                                             m_tuning.settings);
            if (!third) { // first person turns the player tick by tick (the mouse turns it per frame)
                m_yaw += in.look.x * turn_speed() * static_cast<float>(k_tick);
                m_pitch = std::clamp(m_pitch - movement::pitch_delta(in.look.y, m_tuning.fov_deg, m_tuning.settings),
                                     -k_half_pi, k_half_pi);
            }
        } else {
            m_pad_full_time = 0;
        }
        if (third && !in.gamepad) {
            // The mouse as the controls see it: x a yaw rate over the update, y a pitch input.
            const auto& s = m_tuning.settings;
            const float dy = s.invert_y ? m_look_counts.y : -m_look_counts.y;
            in.look = {m_look_counts.x * s.mouse_heading_sensitivity * s.mouse_heading_x_scale / float(k_tick),
                       dy * s.mouse_heading_sensitivity * s.mouse_heading_y_scale};
            m_look_counts = glm::vec2(0.0f);
        }
    }
    in.yaw = m_yaw;
    in.pitch = m_pitch;
    in.graph_driven = m_graph.graph_driven;
    in.graph_velocity = m_graph.graph_velocity;
    in.graph_state = m_graph.graph_state;
    in.third_person = third;
    in.weapon_drawn = m_weapon_drawn;
    m_launched = in;
    std::lock_guard lock(m_mutex);
    m_job_input = in;
    m_job_step = m_launched_step;
    m_job_tuning = m_tuning;
    m_job_watch = m_watch;
    m_job_body = m_body;
    m_job_body_set = std::exchange(m_body_set, false);
    m_job_body_hurt = std::exchange(m_body_hurt, false);
    m_job_knock_next = std::exchange(m_knock_next, glm::vec3(0.0f));
    m_job_knock_next_seconds = std::exchange(m_knock_next_seconds, 0.0f);
    m_job = true;
    m_job_queued = true;
    m_cv.notify_all();
}

bool Player::job_done() {
    std::lock_guard lock(m_mutex);
    return !m_job;
}

void Player::wait() {
    std::unique_lock lock(m_mutex);
    m_cv.wait(lock, [&] { return !m_job; });
}

void Player::tick(const PlayerInput& in, bool step) {
    const float dt = static_cast<float>(k_tick);
    const MovementTuning& t = m_job_tuning;
    const movement::Settings& set = t.settings;
    const float u = units::k_skyrim_unit_m;
    const glm::vec3 feet = m_character->state().position;
    m_world->update(feet);
    m_result.ready = m_world->ready(feet);
    m_result.ran = false;
    if (!step || !m_result.ready) {
        return;
    }
    PlayerStatus& s = m_sim.status;
    if (m_job_body_set) {
        s.health = m_job_body.x;
        s.stamina = m_job_body.y;
    }
    if (m_job_body_hurt) {
        s.health_regen_delay = std::min(std::max(s.health_regen_delay, set.damaged_health_regen_delay), set.health_regen_delay_max);
    }
    // The strongest shove stands (the game adds timer x the value: a fade from full to none).
    const float knock_now = m_knock_seconds > 0.0f ? m_knock_left / m_knock_seconds : 0.0f;
    if (m_job_knock_next_seconds > 0.0f && glm::length(m_job_knock_next) >= glm::length(m_knock) * knock_now) {
        m_knock = m_job_knock_next;
        m_knock_left = m_knock_seconds = m_job_knock_next_seconds;
    }
    const glm::vec2 body_before(s.health, s.stamina);

    // The input pass: a sprint press toggles the request (only with stamina left), a sneak press toggles sneaking.
    if (in.sprint_press && (s.stamina > 0 || s.sprint_request)) {
        s.sprint_request = !s.sprint_request;
    }
    if (in.sneak && !t.riding) { // sneaking is defined as not mounted
        s.sneaking = !s.sneaking;
    }

    // Third person: while the player stood still last tick, the look input circles the camera around it (free
    // rotation); once it moves, the player takes the camera's heading and pitch and the look turns the player,
    // as in first person.
    float yaw = in.yaw, pitch = in.pitch;
    // The Cam3 and Camera1st nodes' heights are read from the pose drawn last frame, like the root they sit on
    // (measured: third_sneak and ground6 sneak, the height starts and ends its blend one frame after the sneak
    // toggles).
    const float b = m_sim.sneak_blend;
    const float blend_rate = dt / std::max(t.sneak_blend_time, 1e-3f);
    m_sim.sneak_blend = std::clamp(m_sim.sneak_blend + (s.sneaking ? blend_rate : -blend_rate), 0.0f, 1.0f);
    const float k = b * b * (3.0f - 2.0f * b);
    m_result.sneak = k;
    camera3::Target target;
    if (in.third_person) {
        const osr::MovementType& mt = s.swimming ? t.swim : s.sprinting ? t.sprint : t.walk_run;
        // A form's skeleton has its own Cam3 (GetThirdPersonPivotHeight reads the node).
        const float form_height = m_form_camera_height.load(std::memory_order_relaxed);
        target.height = (form_height > 0.0f ? form_height : t.camera3_height + (t.sneak_camera3_height - t.camera3_height) * k) / u;
        target.root = feet / u;
        target.position = feet / u;
        target.heading = std::fmod(std::fmod(yaw, 2.0f * std::numbers::pi_v<float>) + 2.0f * std::numbers::pi_v<float>,
                                   2.0f * std::numbers::pi_v<float>);
        target.pitch = -pitch;
        target.weapon_drawn = in.weapon_drawn;
        if (!m_sim.camera_started) {
            m_sim.camera = camera3::ThirdPerson(t.camera3);
            m_sim.camera.begin(target);
            m_sim.camera_started = true;
        }
        // A mount's view turns by itself; the player's yaw is that view, and the mount follows it (below).
        if (const auto handed = m_sim.camera.set_free_rotation(!t.riding, glm::length(m_sim.move) > 0, target)) {
            yaw = handed->heading;
            pitch = -handed->pitch;
            target.heading = handed->heading;
            target.pitch = handed->pitch;
        }
        if (!m_sim.camera.free_rotation()) {
            const bool falling = t.riding && m_character->state().state == physics::ControllerState::in_air;
            yaw += in.look.x * (falling ? t.fall.rotate_while_moving : t.riding ? mt.rotate_while_moving : mt.rotate_in_place[1]) * dt;
            pitch = std::clamp(pitch - movement::pitch_delta(in.look.y, t.fov_deg, set), -k_half_pi,
                               t.riding ? t.max_upward_pitch : k_half_pi);
            target.heading = std::fmod(std::fmod(yaw, 2.0f * std::numbers::pi_v<float>) + 2.0f * std::numbers::pi_v<float>,
                                       2.0f * std::numbers::pi_v<float>);
            target.pitch = -pitch;
        }
    }

    // The movement type the behaviour graph moves with: chosen from last tick's states (a sneaking player that
    // was standing still moves its first tick with the default type).
    const bool graph_sprint = m_sim.graph_sprint;
    const osr::MovementType* attack = t.riding ? nullptr : attack_type(t, in.graph_state);
    const osr::MovementType& type = s.swimming    ? t.swim
                                    : attack       ? *attack
                                    : graph_sprint ? t.sprint
                                    : (s.sneaking && m_sim.was_moving) ? t.sneak
                                                                       : t.walk_run;
    const movement::Speeds speeds = movement::actor_speeds(type, t.body, set);

    // The regeneration pass (at the start of the player's update): its
    // time step is the change of the session clock, a float, so it is the frame time rounded to the clock's
    // spacing and a 0.5 s delay lasts 30 or 31 frames depending on how long the session has run. Each delay
    // counts down first; regeneration runs in the update the delay reaches 0, stamina's not while sprinting.
    {
        const float now = m_sim.process_time;
        if (m_sim.regen_last > now || m_sim.regen_last < 0) {
            m_sim.regen_last = now > 0 && now < 0.3f ? 0.0f : now;
        }
        const float dt_r = now - m_sim.regen_last;
        m_sim.regen_last = now;
        s.process_time = now;
        auto held = [&](float& delay) {
            if (delay > 0) {
                delay = std::max(delay - dt_r, 0.0f);
                return delay > 0;
            }
            return false;
        };
        if (!held(s.health_regen_delay) && s.health < t.max_health && !m_down) {
            s.health = std::min(t.max_health, s.health + movement::regen_per_second(t.max_health, t.heal_rate,
                                                                                    100.0f, false, set) * dt_r);
        }
        if (!held(s.stamina_regen_delay) && s.stamina < t.max_stamina && !s.sprinting) {
            s.stamina = std::min(t.max_stamina, s.stamina + movement::regen_per_second(
                                                                t.max_stamina, t.stamina_rate, t.stamina_rate_mult,
                                                                false, set) * dt_r);
        }
    }
    // Stamina: sprinting drains it like damage: any loss sets the delay to at least
    // fDamagedStaminaRegenDelay, and while the sprint state lasts at no stamina the delay is set to
    // fOutOfBreathStaminaRegenDelay every update.
    if (s.sprinting) {
        s.stamina -= movement::sprint_drain_per_second(t.body.equipped_weight, set) * dt;
        s.stamina_regen_delay = std::min(std::max(s.stamina_regen_delay, set.damaged_stamina_regen_delay),
                                         set.stamina_regen_delay_max);
        if (s.stamina <= 0) {
            s.stamina = 0;
            s.stamina_regen_delay =
                std::min(std::max(s.stamina_regen_delay, set.out_of_breath_stamina_regen_delay), set.stamina_regen_delay_max);
            s.sprint_request = false;
        }
    }

    // The requested velocity, and one controller step. The jump button does nothing while sprinting.
    movement::Motion motion = movement::requested_motion(speeds, in.move, in.run, in.gamepad, set);
    float facing = yaw;
    if (t.riding) {
        // Planner Horse Controls: the keys ask for a heading from the view, W where it looks, A left of it;
        // the speed is the level's, walk at 1 and run at 2, and the mount turns there at the level's rate.
        constexpr float k_tau = 2.0f * std::numbers::pi_v<float>;
        float& heading = s.mount_heading;
        const float level = in.run ? 2.0f : 1.0f;
        const bool moving = glm::length(m_sim.move) >= 5.0f;
        const bool asked = glm::length(in.move) >= 0.1f;
        const osr::MovementType& mt = s.swimming ? t.swim : graph_sprint ? t.sprint : t.walk_run;
        const float rate = moving ? level * mt.rotate_while_moving * 0.5f : mt.rotate_in_place[in.run ? 1 : 0];
        float off = 0;
        if (asked) {
            off = std::remainder(yaw + std::atan2(in.move.x, in.move.y) - heading, k_tau);
            heading += std::clamp(off, -rate * dt, rate * dt);
        }
        if (moving) { // the view stays within its angle of a moving mount
            const float view = std::remainder(yaw - heading, k_tau);
            yaw = heading + std::clamp(view, -t.max_angle_before_turn, t.max_angle_before_turn);
        }
        // Left and right speeds are 0: it gathers speed as it comes to face the way it's asked (inferred, spec).
        const float full = graph_sprint ? speeds.forward[1] : speeds.forward[in.run ? 1 : 0];
        motion.direction = 0;
        motion.speed = asked ? full * std::max(std::cos(off), 0.0f) : 0.0f;
        facing = heading;
    }
    const bool driven = in.graph_driven && !t.riding && !s.swimming;
    if (driven) {
        motion.speed = glm::length(in.graph_velocity);
        motion.direction = motion.speed > 0.0f ? std::atan2(in.graph_velocity.x, in.graph_velocity.y) : 0.0f;
    }
    if ((in.third_person || t.riding) && !driven) {
        // Third person, the controls' movement agent: the request goes through a buffer and comes out
        // fControllerBufferDepth later (8 updates at 60 Hz), and the commanded velocity (facing space) then moves
        // toward it by at most the movement type's forward run speed per dampen time, per second.
        const movement::MoveBuffer::Out buffered = t.riding ? movement::MoveBuffer::Out{0.0f, glm::vec2(0.0f, motion.speed)}
                                                            : m_sim.buffer.push(motion.direction, motion.speed, dt, set);
        const glm::vec2 want = buffered.velocity;
        const glm::vec2 now = m_sim.move;
        glm::vec2 out = want;
        const float now_speed = glm::length(now);
        if (buffered.freeze_end) {
            // The end of a large direction change: the new direction at the current speed, no ramp or slew.
            out = glm::vec2(std::sin(buffered.direction), std::cos(buffered.direction)) * now_speed;
        } else if (now_speed - glm::length(want) > set.max_move_ramp_down) {
            out = glm::length(want) > 0 ? want * ((now_speed - set.max_move_ramp_down) / glm::length(want))
                                        : now * ((now_speed - set.max_move_ramp_down) / now_speed);
        }
        if (t.third_person_dampen_time > 0 && !buffered.freeze_end) {
            const float ramp = speeds.forward[1] / t.third_person_dampen_time * dt;
            const glm::vec2 d = out - now;
            const float len = glm::length(d);
            out = len <= ramp ? out : now + d * (ramp / len);
        }
        const float speed = glm::length(out);
        m_sim.move = speed <= 5.0f ? glm::vec2(0.0f) : out;
        motion.speed = speed <= 5.0f ? 0.0f : speed;
        if (motion.speed > 0) {
            motion.direction = std::atan2(out.x, out.y);
        }
    } else {
        m_sim.move = motion.speed > 0 ? glm::vec2(std::sin(motion.direction), std::cos(motion.direction)) * motion.speed
                                      : glm::vec2(0.0f);
    }
    physics::ControllerInput ci;
    {
        // Facing space (x right, y ahead); a swimmer's is turned by the view pitch, so swimming forward while
        // looking down dives (positive pitch looks down in the game).
        glm::vec3 local(std::sin(motion.direction), std::cos(motion.direction), 0.0f);
        if (s.swimming && !t.riding) { // horses can't dive
            const float down = -pitch;
            local = glm::vec3(local.x, local.y * std::cos(down), -local.y * std::sin(down));
        }
        const float cy = std::cos(facing), sy = std::sin(facing);
        const glm::vec3 world(local.x * cy + local.y * sy, -local.x * sy + local.y * cy, local.z);
        ci.desired = world * (motion.speed * u);
        ci.facing = glm::vec2(sy, cy);
    }
    const glm::vec3 water_feet = m_water_feet.value_or(feet);
    m_water_feet.reset();
    ci.water_height = water_height(glm::vec2(water_feet));
    ci.water_feet_z = feet.z; // the swim level is the controller's own (deep or at the surface)
    // The cell water's current, while the feet are under its surface.
    if (std::isfinite(ci.water_height) && water_feet.z < ci.water_height) {
        if (const auto c = current_at(water_feet)) {
            const physics::CharacterState& cs = m_character->state();
            const bool falling_fast = cs.state == physics::ControllerState::in_air &&
                                      cs.velocity.z / u < -set.jump_fall_velocity_min;
            // The water update runs before this frame's movement: the cap uses last frame's movement velocity.
            ci.external = movement::applied_current(*c, m_sim.world_move, speeds.forward[1], s.swimming, falling_fast,
                                                    set) * u;
        }
    }
    if (m_knock_left > 0.0f) {
        ci.external += m_knock * (m_knock_left / m_knock_seconds);
        m_knock_left -= dt;
    }
    ci.body_height = t.body_height;
    ci.jump = in.jump && (!s.sprinting || t.riding); // the horse's graph decides, and nothing stops a sprinting one
    ci.jump_speed = movement::jump_speed(t.body.scale, s.swimming, t.gravity / u, set) * u;
    ci.gravity = t.gravity;
    ci.falling_gravity_mult = set.in_air_falling_gravity_mult;
    ci.air_control = movement::air_control_gain(set);
    ci.unsupported_gravity_mult = set.unsupported_gravity_mult;
    ci.unsupport_cast = set.unsupport_cast_length * u;
    m_character->step(ci, dt);
    m_result.state = m_character->state();
    m_result.refs.clear();
    for (uint32_t id : m_job_watch) {
        if (const auto p = m_world->ref_position(id)) {
            m_result.refs.push_back({id, *p});
        }
    }
    // Fallen through the ground: an unsupported actor more than 30 units below the drawn land is put back on it
    // (the game after the controller update, exteriors).
    if (!m_result.state.supported) {
        const glm::vec3 at = m_result.state.position;
        const float land = land_height(glm::vec2(at));
        if (std::isfinite(land) && land - at.z > 30.0f * u) {
            m_character->place(glm::vec3(at.x, at.y, land));
            m_result.state = m_character->state();
        }
    }
    m_result.yaw = yaw;
    m_result.pitch = pitch;
    if (in.third_person) {
        // The camera follows the player's 3D root, which still shows last frame's position; its collision cast
        // starts above where the player is now.
        target.position = m_result.state.position / u;
        const uint64_t layers = t.camera_layers;
        const camera3::SphereCast cast = [&](const glm::vec3& from, const glm::vec3& to,
                                             float radius) -> std::optional<glm::vec3> {
            if (layers == 0) {
                return std::nullopt;
            }
            const auto hit = m_world->sphere_cast(from * u, to * u, radius * u, layers);
            return hit ? std::optional(hit->point / u) : std::nullopt;
        };
        m_sim.camera.zoom(in.zoom);
        m_sim.camera.update(target, m_sim.camera.free_rotation() ? in.look : glm::vec2(0.0f), dt, cast);
        m_result.camera_zoom = m_sim.camera.zoom_level();
        m_result.camera = m_sim.camera.position() * u;
        m_result.camera_yaw = m_sim.camera.yaw();
        m_result.camera_pitch = -m_sim.camera.pitch();
        m_result.camera_free = m_sim.camera.free_rotation();
    }

    // Sprinting continues or starts only while asked for, moving forward at running speed (last tick's), with
    // stamina; the graph switches to it a tick later. Nothing is decided while jumping or in the air: a request
    // made then waits for the landing tick (measured).
    bool sprint = graph_sprint && s.stamina > 0;
    if (m_result.state.state == physics::ControllerState::on_ground) {
        const movement::Speeds normal = movement::actor_speeds(t.walk_run, t.body, set);
        sprint = s.sprint_request && (graph_sprint || m_sim.last_speed >= movement::jog_speed(normal, set)) &&
                 movement::can_run(t.body) && !s.swimming && in.move.y > 0 &&
                 std::abs(in.move.x) < set.sprint_stop_threshold && s.stamina > 0;
        if (!sprint) {
            s.sprint_request = false;
        }
    }

    // (Health regenerated in the regeneration pass above: HealRate percent per second, held while drowning damage
    // keeps fDamagedHealthRegenDelay running; a fall's damage starts no delay, measured.)
    // Breath runs down while the head is under water (0.875 of the body height) and refills at once above it;
    // out of breath, health drains at fActorSwimBreathDamage of the maximum per second.
    const float max_breath = set.actor_swim_breath_base + set.actor_swim_breath_mult * 50.0f;
    s.swimming = m_result.state.swimming;
    s.submersion = m_result.state.submersion;
    // The head test uses where the actor is, also on the step after a teleport (Measured: water2 breath, a setpos
    // from the surface to the lake bed starts the breath timer on the first step).
    float head_submersion = s.submersion;
    if (const float wh = water_height(glm::vec2(feet)); std::isfinite(wh) && t.body_height > 0) {
        head_submersion = std::clamp((wh - feet.z) / t.body_height, 0.0f, 1.0f);
    }
    if (head_submersion >= 0.875f) {
        s.breath -= dt;
        if (s.breath < 0) {
            s.breath = 0;
            s.health = std::max(0.0f, s.health - set.actor_swim_breath_damage * t.max_health * dt);
            s.health_regen_delay = std::min(std::max(s.health_regen_delay, set.damaged_health_regen_delay),
                                            set.health_regen_delay_max);
        }
    } else {
        s.breath = max_breath;
    }
    s.last_fall_damage = 0;
    if (m_result.state.landed && s.submersion < 0.7f && !t.riding) {
        s.last_fall_damage = movement::fall_damage(m_result.state.fall_distance / u, set);
        s.health = std::max(0.0f, s.health - s.last_fall_damage);
        s.falls_hurt += s.last_fall_damage > 0.0f ? 1u : 0u;
    }
    s.speed = motion.speed;
    // The one-tick pipelines: the sprint flag follows the graph's state, the graph follows this tick's decision.
    // The flag (and its stamina drain) outlasts a stopped sprint by 11 ticks: it goes 12 frames after the request
    // clears, whatever stopped it (measured: releasing the keys, running dry, strafing).
    constexpr int k_sprint_tail_ticks = 11;
    if (graph_sprint) {
        m_sim.sprint_tail = k_sprint_tail_ticks;
        s.sprinting = true;
    } else {
        s.sprinting = m_sim.sprint_tail > 0;
        m_sim.sprint_tail = std::max(0, m_sim.sprint_tail - 1);
    }
    m_sim.graph_sprint = sprint;
    s.graph_sprint = sprint;
    if (!driven) { // the channels hold while the graph drives
        s.asked = m_sim.move;
    }
    m_sim.was_moving = motion.speed > 0;
    m_sim.world_move = ci.desired / u;
    m_sim.last_speed = glm::length(glm::vec2(m_result.state.velocity)) / u;
    // The session clock gains the frame's world time after the player's update (the process lists job), in
    // single precision, and wraps to 0 above 100000 s.
    // Only while AI processing is on: ToggleAI stops the process lists, clock included (RE: ToggleAI
    // flips the game, which guards the increment in; measured: river4 and water3 run with
    // "ai off" and their clock stays where it was set).
    if (m_sim.ai_processing) {
        m_sim.process_time += static_cast<float>(k_tick);
        if (!(m_sim.process_time <= 100000.0f)) {
            m_sim.process_time = 0;
        }
    }
    m_result.status = s;
    m_result.body_change = glm::vec2(s.health, s.stamina) - body_before;
    m_result.ran = true;
}

void Player::frame(double dt, const PlayerInput& live) {
    m_live = live;
    m_jump_latch = m_jump_latch || live.jump;
    m_sneak_latch = m_sneak_latch || live.sneak;
    m_sprint_latch = m_sprint_latch || live.sprint_press;
    if (m_paused) {
        return;
    }
    m_into += dt;
    while (m_into >= k_tick) {
        if (!job_done()) {
            if (m_into < 2.0 * k_tick && !m_lockstep) {
                break; // the tick is a little late: show the last one rather than stall the frame
            }
            wait(); // slow frames: catch up
        }
        if (!m_result.ran) {
            // Held by a script, or collision is not in yet: time stands still on the last tick, and the next
            // attempt comes a tick later. A step that could not run is tried again with the same input.
            m_waiting = !m_result.ready;
            if (m_launched_step && m_launched_scripted) {
                m_script.push_front(m_launched);
            } else if (m_launched_step) {
                m_jump_latch = m_jump_latch || m_launched.jump;
                m_sneak_latch = m_sneak_latch || m_launched.sneak;
                m_sprint_latch = m_sprint_latch || m_launched.sprint_press;
            }
            m_prev = m_curr;
            m_prev_sneak = m_curr_sneak;
            m_into = 0;
            launch();
            break;
        }
        m_waiting = false;
        m_prev = m_curr;
        m_curr = m_result.state;
        m_prev_sneak = m_curr_sneak;
        m_curr_sneak = m_result.sneak;
        m_status = m_result.status;
        m_body_change += m_result.body_change;
        m_result.ran = false;
        ++m_ticks;
        if (m_launched.third_person) {
            m_yaw = m_result.yaw;
            m_pitch = m_result.pitch;
            m_prev_camera = m_curr_camera;
            m_curr_camera = m_result.camera;
            m_prev_camera_yaw = m_camera_yaw;
            m_prev_camera_pitch = m_camera_pitch;
            m_camera_yaw = m_result.camera_yaw;
            m_camera_zoom = m_result.camera_zoom;
            m_camera_pitch = m_result.camera_pitch;
            m_camera_free = m_result.camera_free;
        }
        const glm::vec3 feet_units = m_curr.position / units::k_skyrim_unit_m;
        if (!m_lag_on && !m_launched.third_person) {
            m_lag.begin(feet_units);
        }
        m_lag_on = !m_launched.third_person;
        m_prev_lag = m_curr_lag;
        m_curr_lag = m_lag_on ? m_lag.step(feet_units, float(k_tick), m_status.riding, {}) * units::k_skyrim_unit_m
                              : glm::vec3(0);
        if (m_tracing) {
            const float eye = eye_height(m_curr_sneak);
            const glm::vec3 view = m_launched.third_person ? m_curr_camera : m_curr.position + glm::vec3(0, 0, eye);
            m_trace.push_back({m_ticks, m_result.refs, m_curr, m_status, m_result.yaw, m_result.pitch, eye, view,
                               m_launched.third_person});
        }
        m_into -= k_tick;
        launch();
    }
    const float a = m_lockstep ? m_lockstep_shown : static_cast<float>(std::clamp(m_into / k_tick, 0.0, 1.0));
    m_shown_fraction = a;
    m_shown_feet = glm::mix(m_prev.position, m_curr.position, a);
    m_shown_camera = glm::mix(m_prev_camera, m_curr_camera, a);
    m_shown_sneak = glm::mix(m_prev_sneak, m_curr_sneak, a);
    m_shown_lag = glm::mix(m_prev_lag, m_curr_lag, a);
}

void Player::teleport(const glm::vec3& feet) {
    wait();
    m_result.ran = false;
    // The water's current comes from where the actor was before the setpos for the first step (Measured: water2
    // fall_into_water, the lake current on the first step after a setpos from under the water into the air); the
    // swim level already follows the new position (water6 fall_into_water, surface gravity on that step).
    m_water_feet = m_character->state().position;
    m_character->set_position(feet);
    m_curr = m_character->state();
    m_curr.position = feet;
    m_prev = m_curr;
    m_shown_feet = feet;
    m_into = 0;
    m_waiting = true;
    m_jump_latch = m_sneak_latch = m_sprint_latch = false;
    m_sim.graph_sprint = m_sim.was_moving = false;
    m_sim.sprint_tail = 0;
    m_sim.last_speed = 0;
    m_sim.move = glm::vec2(0.0f);
    m_sim.world_move = glm::vec3(0.0f);
    m_sim.status.sprinting = m_sim.status.graph_sprint = m_sim.status.sprint_request = false;
    m_status = m_sim.status;
    launch();
}

void Player::carry(physics::World& world, std::shared_ptr<const osw::World> water,
                   const std::function<glm::vec3(const glm::vec3&)>& point, float angle) {
    wait();
    m_result.ran = false;
    const float c = std::cos(angle), s = std::sin(angle);
    auto turn = [&](const glm::vec3& v) { return glm::vec3(c * v.x - s * v.y, s * v.x + c * v.y, v.z); };
    const physics::CharacterState was = m_character->state();
    m_world = &world;
    m_water = std::move(water);
    m_character = std::make_unique<physics::Character>(world, m_tuning.controller, point(was.position));
    m_character->set_velocity(turn(was.velocity));
    for (physics::CharacterState* st : {&m_prev, &m_curr}) {
        st->position = point(st->position);
        st->velocity = turn(st->velocity);
    }
    m_shown_feet = point(m_shown_feet);
    m_prev_camera = point(m_prev_camera);
    m_curr_camera = point(m_curr_camera);
    m_shown_camera = point(m_shown_camera);
    m_yaw -= angle;
    m_camera_yaw -= angle;
    m_prev_camera_yaw -= angle;
    m_sim.world_move = turn(m_sim.world_move);
    m_water_feet.reset();
    m_waiting = true; // until its collision is in; the far side streams ahead, so only a distant door waits
    launch();
}

void Player::watch(std::vector<uint32_t> form_ids) {
    wait();
    m_watch = std::move(form_ids);
}

void Player::set_ai_processing(bool on) {
    wait();
    m_sim.ai_processing = on;
}

void Player::set_process_time(float seconds) {
    wait();
    m_sim.process_time = seconds;
    m_sim.regen_last = seconds;
}

void Player::set_breath(float seconds) {
    wait();
    m_sim.status.breath = seconds;
}

void Player::restore() {
    wait();
    m_sim.status.stamina = m_tuning.max_stamina;
    m_sim.status.health = m_tuning.max_health;
    m_sim.status.stamina_regen_delay = 0;
    m_status = m_sim.status;
}

void Player::queue_input(const PlayerInput& input, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        PlayerInput in = input;
        in.jump = input.jump && i == 0;
        in.sneak = input.sneak && i == 0;
        in.sprint_press = input.sprint_press && i == 0;
        m_script.push_back(in);
    }
}

size_t Player::scripted() {
    // The tick in flight counts until its result has been taken.
    return m_script.size() + (m_launched_scripted ? 1 : 0);
}

} // namespace spud::engine
