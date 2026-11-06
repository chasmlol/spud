#include "engine/app_internal.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/units.hpp"
#include "formats/okc.hpp"
#include "game/camera_paths.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <random>

// The kill camera: VATS playback, mode 4. The player's kill moves, for now.

namespace spud::engine {

namespace {

constexpr float k_u = units::k_skyrim_unit_m;

} // namespace

const okc::Model* App::kill_camera_model(const std::string& path) {
    if (!m_kill_cameras_loaded) {
        m_kill_cameras_loaded = true;
        const auto file = m_opt.cache / "rules" / "cameras.okc";
        std::error_code missing;
        if (std::filesystem::exists(file, missing)) {
            m_kill_cameras = std::make_unique<okc::Cameras>(okc::read(read_file(file)));
        } else {
            log::warn("no rules/cameras.okc: run the importer");
        }
    }
    if (m_kill_cameras) {
        for (const okc::Model& m : m_kill_cameras->models) {
            if (m.path == path) {
                return &m;
            }
        }
    }
    return nullptr;
}

// How far a camera can stand on a side of the subject (515 to 518) and whether it would see the target from there (522 to
// 525): 0 right, 1 left, 2 back, 3 front.
float App::vats_area_free(uint32_t subject, int side) {
    game::CombatBody body;
    if (!m_physics || !m_game->combat().world().body(subject, body)) {
        return 0.0f;
    }
    const float height = m_game->settings().number("fVATSSmartCameraCheckHeight", 96.0f);
    const float turn = glm::radians(body.heading) + std::array{glm::half_pi<float>(), 3.0f * glm::half_pi<float>(), glm::pi<float>(), 0.0f}[size_t(side)];
    const glm::vec3 direction(std::sin(turn), std::cos(turn), 0.0f);
    const glm::vec3 from = glm::vec3(body.position[0], body.position[1], body.position[2] + height) * k_u;
    const auto hit = m_physics->raycast(from, direction, 10000.0f * k_u);
    return hit ? hit->distance / k_u : FLT_MAX;
}

float App::vats_target_visible(uint32_t subject, uint32_t target, int side) {
    game::CombatBody body, other;
    game::CombatWorld& world = m_game->combat().world();
    if (!m_physics || !target || !world.body(subject, body) || !world.body(target, other)) {
        return 0.0f;
    }
    const game::Settings& s = m_game->settings();
    const float height = s.number("fVATSSmartCameraCheckHeight", 96.0f);
    const float step = s.number("fVATSSmartCameraCheckStepDistance", 128.0f);
    const int steps = int(s.number("fVATSSmartCameraCheckStepCount", 5.0f));
    const float turn = glm::radians(body.heading) + std::array{glm::half_pi<float>(), 3.0f * glm::half_pi<float>(), glm::pi<float>(), 0.0f}[size_t(side)];
    const glm::vec3 direction(std::sin(turn), std::cos(turn), 0.0f);
    const glm::vec3 base(body.position[0], body.position[1], body.position[2] + height);
    const glm::vec3 centre(other.position[0], other.position[1], other.position[2] + other.height * 0.5f);
    float clear = 0.0f;
    for (int i = 1; i <= steps; ++i) {
        const glm::vec3 from = base + direction * (step * float(i));
        const glm::vec3 to = centre - from;
        const float length = glm::length(to);
        if (length <= 0.0f) {
            break;
        }
        const auto hit = m_physics->raycast(from * k_u, to / length, length * k_u);
        if (hit && hit->distance / k_u <= length * 0.85f) {
            break;
        }
        clear = step * float(i);
    }
    return clear;
}

// The game with force: a kill move. The path is walked here, not at the first update, as a first-person
// shot decides which graph plays the pair.
bool App::ranged_odds(uint32_t target, const game::Item* weapon) {
    game::Combat& combat = m_game->combat();
    game::CombatWorld& world = combat.world();
    const game::Settings& s = m_game->settings();
    game::CombatBody a, b;
    if (!world.body(game::k_player_ref, a) || !world.body(target, b)) {
        return false;
    }
    const auto av = [&](uint32_t ref, int v) { return m_game->actor_values(ref).current(v); };
    const float far = s.number("fVATSRangedLongDistance", 2500.0f);
    const float distance = glm::distance(glm::vec3(a.position[0], a.position[1], a.position[2]), glm::vec3(b.position[0], b.position[1], b.position[2]));
    const int level = m_game->magic_target(game::k_player_ref).level(), other = m_game->magic_target(target).level();
    const float full = m_game->actor_values(game::k_player_ref).base(game::av_health);
    if (distance >= far || combat.should_attack_kill(target, game::k_player_ref) || level < other ||
        (full > 0.0f && av(game::k_player_ref, game::av_health) / full * 100.0f < s.number("fVATSRangedLowHealthPercent", 5.0f)) ||
        m_game->race_size(combat.fighter(target).npc) >= 2) {
        return true;
    }
    const float no_threat = s.number("fVATSRangedPercentTargetNoThreat", 10.0f);
    const bool sneaking = m_player && m_player->status().sneaking;
    const uint8_t type = weapon ? weapon->animation_type : 0;
    float penalty = 0.0f;
    if (sneaking && (type == 7 || type == 9) && world.detection(target, game::k_player_ref) <= 0) {
        penalty = s.number("fVATSRangedPercentSneakKill", 20.0f);
    } else if (!(combat.fighter(target).combat_target == game::k_player_ref && combat.fighter(target).attack_state != game::AttackState::none)) {
        penalty = no_threat;
    }
    if (float(other) < float(level) * s.number("fVATSRangedTargetLowLevelMult", 0.3f)) {
        penalty += no_threat;
    }
    static std::mt19937 random{uint32_t(std::chrono::steady_clock::now().time_since_epoch().count())};
    const float roll = float(random() >> 8) / float(1u << 24) * 100.0f;
    return roll <= std::max(distance / (far * 0.01f), s.number("fVATSRangedPercentMin", 50.0f)) - penalty;
}

// the shot's detrimental health effects, added up; a kill that needs the duration doesn't count here.
bool App::spell_kills(uint32_t target, const game::Spell& spell, bool dual) {
    float total = 0.0f;
    for (const game::SpellEffect& e : spell.effects) {
        const game::MagicEffect* effect = m_game->magic().effect(e.effect);
        if (effect && effect->actor_value == game::av_health && (effect->flags & game::effect_detrimental) && (e.duration == 0 || spell.cast_type == game::cast_concentration)) {
            total += e.magnitude;
        }
    }
    (void)dual;
    return total > 0.0f && m_game->actor_values(target).current(game::av_health) - total <= 0.0f;
}

// The crosshair's actor, else who the shot would meet: run along the aim.
uint32_t App::aim_target(const game::LaunchData& shot) {
    game::Combat& combat = m_game->combat();
    if (m_crosshair_ref && m_game->ai().agent(m_crosshair_ref) && !combat.fighter(m_crosshair_ref).dead()) {
        return m_crosshair_ref;
    }
    const float c = std::cos(shot.pitch);
    const glm::vec3 from(shot.origin[0], shot.origin[1], shot.origin[2]);
    const glm::vec3 aim(std::sin(shot.heading) * c, std::cos(shot.heading) * c, std::sin(shot.pitch));
    uint32_t best = 0;
    float nearest = FLT_MAX;
    for (const uint32_t ref : combat.fighting()) {
        game::CombatBody b;
        if (ref == game::k_player_ref || combat.fighter(ref).dead() || !combat.world().body(ref, b) || !b.loaded) {
            continue;
        }
        const glm::vec3 centre(b.position[0], b.position[1], b.position[2] + b.height * 0.5f);
        const float along = glm::dot(centre - from, aim);
        const float off = glm::distance(from + aim * along, centre);
        if (along > 0.0f && along < nearest && off <= std::max(b.radius, b.height * 0.5f)) {
            best = ref;
            nearest = along;
        }
    }
    return best;
}

// The game for a bow, a crossbow and an aimed spell, at the moment the shot goes.
void App::try_ranged_kill_camera(game::LaunchData& shot) {
    game::Combat& combat = m_game->combat();
    if (m_kill_camera.active || m_mount.ref || m_player_beast || !m_player || shot.use_origin) { // use_origin: a chain's hop
        return;
    }
    const game::Spell* spell = shot.spell ? m_game->magic().spell(shot.spell) : nullptr;
    if (shot.spell && !spell) {
        return;
    }
    const game::Gear gear = combat.gear(game::k_player_ref);
    const game::Item* weapon = spell ? nullptr : gear.right.item;
    if (!spell && (!weapon || (weapon->animation_type != 7 && weapon->animation_type != 9))) {
        return;
    }
    std::vector<uint32_t> enemies;
    for (const uint32_t ref : combat.fighting()) {
        if (ref != game::k_player_ref && !combat.fighter(ref).dead() && combat.fighter(ref).combat_target == game::k_player_ref) {
            enemies.push_back(ref);
        }
    }
    // 1.4: a fire and forget spell on the caster with an area: all that fight the player must die of it.
    const bool area = spell && spell->delivery == game::delivery_self && spell->cast_type == game::cast_fire_and_forget &&
                      std::ranges::any_of(spell->effects, [](const game::SpellEffect& e) { return e.area > 0; });
    uint32_t target = 0;
    if (area) {
        if (enemies.empty() || !std::ranges::all_of(enemies, [&](uint32_t ref) { return spell_kills(ref, *spell, shot.dual_cast); })) {
            return;
        }
        game::CombatBody self, body;
        float nearest = 2500.0f;
        for (const uint32_t ref : enemies) {
            if (combat.world().body(game::k_player_ref, self) && combat.world().body(ref, body)) {
                const float d = glm::distance(glm::vec3(self.position[0], self.position[1], self.position[2]),
                                              glm::vec3(body.position[0], body.position[1], body.position[2]));
                if (d < nearest) {
                    nearest = d;
                    target = ref;
                }
            }
        }
        if (!target) {
            return;
        }
    } else {
        // One enemy, as bVATSMultipleCombatants is off.
        if (enemies.size() > 1) {
            return;
        }
        // Concentration has no odds.
        target = aim_target(shot);
        if (!target || combat.fighter(target).dead() || (!spell && weapon->animation_type == 7 && shot.power < 0.8f)) {
            return;
        }
        const bool kills = spell ? spell_kills(target, *spell, shot.dual_cast) : combat.predict_kill(game::k_player_ref, target);
        if (!kills || (!shot.concentration && !ranged_odds(target, weapon))) {
            return;
        }
    }
    game::VatsEntry& v = m_game->vats();
    v.delivery = spell ? int(spell->delivery) : 0;
    v.casting = spell ? int(spell->cast_type) : 0;
    v.projectile_flags = 0;
    const game::CameraPath* path = pick_kill_camera(target, spell ? 3 : 4);
    if (!path) {
        v.active = false; // the tree is the last gate: every Exit path says no here
        return;
    }
    start_kill_camera(target, path);
    m_kill_camera.ranged = true;
    m_kill_camera.limit = m_game->settings().number("fVATSSafetyMaxTimeRanged", 6.0f);
    if (m_player->view() == View::first_person && !(path->shots.front().flags & game::CameraShot::first_person)) {
        m_kill_move_view = true;
        m_player->set_view(View::third_person);
    }
    shot.desired_target = target;
    shot.always_hit = true;
    if (!spell) {
        shot.power = 1.0f; // 3.3
    }
}

const game::CameraPath* App::pick_kill_camera(uint32_t target, int action) {
    game::VatsEntry& v = m_game->vats();
    game::CombatBody a, b;
    game::CombatWorld& world = m_game->combat().world();
    v.active = true;
    v.pairing = true;
    v.action = action;
    v.first_person = m_player && m_player->view() == View::first_person;
    v.distance = world.body(game::k_player_ref, a) && world.body(target, b)
                     ? glm::distance(glm::vec3(a.position[0], a.position[1], a.position[2]), glm::vec3(b.position[0], b.position[1], b.position[2]))
                     : 0.0f;
    const game::CameraPath* path = m_game->camera_paths().pick(game::k_player_ref, target);
    v.pairing = false;
    return path;
}

void App::start_kill_camera(uint32_t target, const game::CameraPath* path) {
    KillCamera k;
    k.active = true;
    k.target = target;
    k.path = path;
    k.limit = 0.0f;
    if (path) {
        for (const game::CameraShot& s : path->shots) {
            k.limit += s.max_time > 0.001f ? s.max_time : m_game->settings().number("fVATSSafetyMaxTime", 20.0f);
        }
    }
    if (k.limit <= 0.0f) {
        k.limit = m_game->settings().number("fVATSSafetyMaxTime", 20.0f);
    }
    k.fov = m_camera.fov_y;
    m_kill_camera = k;
    if (m_ui) {
        m_ui->hud_mode("VATSPlayback", true);
    }
}

void App::end_kill_camera() {
    if (!m_kill_camera.active) {
        return;
    }
    m_time_rate = m_player_rate = 1.0f;
    m_game->projectiles().tracked = 0;
    m_game->projectiles().player_rate = 1.0f;
    if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
        actors->set_graph_rates(1.0f, 1.0f);
    }
    m_camera.fov_y = m_kill_camera.fov;
    if (m_sky && !m_kill_camera.image_space.empty()) {
        m_sky->remove_modifier(m_kill_camera.image_space, m_game->settings().number("fVATSImageSpaceTransitionTime", 0.15f));
    }
    m_kill_camera = {};
    m_game->vats().active = false;
    if (m_ui) {
        m_ui->hud_mode("VATSPlayback", false);
    }
}

// VATS_NextShot.
void App::next_kill_shot() {
    KillCamera& k = m_kill_camera;
    k.running = false;
    ++k.shot;
}

// the anchors of a shot that doesn't follow its nodes are taken now.
void App::begin_kill_shot(const game::CameraShot& s) {
    KillCamera& k = m_kill_camera;
    const game::Settings& settings = m_game->settings();
    k.running = true;
    k.max = s.max_time > 0.001f ? s.max_time : settings.number("fVATSSafetyMaxTime", 20.0f);
    k.min = std::max({k.min, s.min_time, settings.number("fVATSCameraMinTime", 0.0f)});
    k.started = k.elapsed;
    k.model = s.model.empty() ? nullptr : kill_camera_model(s.model);
    if (!s.model.empty() && !k.model) {
        next_kill_shot(); // a model that won't do skips the shot
        return;
    }
    k.location = kill_shot_node(s.location);
    k.look = kill_shot_node(s.look);
    if (m_sky && s.image_space) {
        const forms::FormEntry* e = m_game->db().find(s.image_space);
        const std::string name = e ? std::string(forms::Form(m_game->db(), *e).editor_id()) : std::string();
        if (name != k.image_space) {
            const float fade = settings.number("fVATSImageSpaceTransitionTime", 0.15f);
            if (!k.image_space.empty()) {
                m_sky->remove_modifier(k.image_space, fade);
            }
            m_sky->apply_modifier(name, 1.0f, k.image_space.empty() ? 0.0f : fade);
            k.image_space = name;
        }
    }
}

// The node a shot hangs on: body part 0's, which is the torso; the root with none (5, Nodes). Units.
glm::vec3 App::kill_shot_node(int place) {
    if (place == game::CameraShot::projectile) {
        if (const game::Projectile* p = m_game->projectiles().find(m_kill_camera.projectile)) {
            return glm::vec3(p->position[0], p->position[1], p->position[2]);
        }
        place = game::CameraShot::attacker; // before the launch; a bound node holds where it last was
    }
    const uint32_t ref = place == game::CameraShot::target && m_kill_camera.target ? m_kill_camera.target : game::k_player_ref;
    ActorSet* actors = m_space ? m_space->actors() : nullptr;
    glm::vec3 at(0.0f);
    float heading = 0.0f;
    if (actors && actors->node_world(ref == game::k_player_ref ? ActorSet::k_player_body : ref, m_game->body_part_node(ref), at)) {
        return at / k_u;
    }
    if (actors && actors->feet(ref == game::k_player_ref ? ActorSet::k_player_body : ref, at, heading)) {
        return at / k_u;
    }
    return glm::vec3(0.0f);
}

// position from the location, view at the look point, then the
// model's rotation in camera space.
void App::place_kill_camera(const game::CameraShot& s) {
    KillCamera& k = m_kill_camera;
    if (!k.model || k.model->frames.empty()) {
        return;
    }
    ActorSet* actors = m_space ? m_space->actors() : nullptr;
    if (!actors) {
        return;
    }
    const float time = (s.flags & game::CameraShot::from_zero ? k.elapsed - k.started : k.elapsed) / okc::k_step;
    const size_t last = k.model->frames.size() - 1;
    const size_t i = std::min(size_t(std::max(time, 0.0f)), last), j = std::min(i + 1, last);
    const float t = std::clamp(time - float(i), 0.0f, 1.0f);
    const okc::Frame& a = k.model->frames[i];
    const okc::Frame& b = k.model->frames[j];
    const auto mix = [t](const std::array<float, 3>& x, const std::array<float, 3>& y) {
        return glm::mix(glm::vec3(x[0], x[1], x[2]), glm::vec3(y[0], y[1], y[2]), t);
    };
    glm::vec3 feet(0.0f);
    float heading = 0.0f;
    actors->feet(ActorSet::k_player_body, feet, heading);
    const float c = std::cos(heading), n = std::sin(heading);
    const glm::mat3 turn(glm::vec3(c, -n, 0.0f), glm::vec3(n, c, 0.0f), glm::vec3(0, 0, 1));
    const glm::vec3 anchor = s.flags & game::CameraShot::position_follows ? kill_shot_node(s.location) : k.location;
    glm::vec3 position = anchor + turn * mix(a.translation, b.translation);

    glm::vec3 look = s.flags & game::CameraShot::rotation_follows ? kill_shot_node(s.look) : k.look;
    if (s.percent > 0.0f) {
        look = glm::mix(kill_shot_node(game::CameraShot::attacker), kill_shot_node(game::CameraShot::target), s.percent * 0.01f);
    }
    // CollideCameraPosition: a sphere from the location node, radius fCameraCasterBleedOutSize.
    if (m_physics) {
        const float radius = 5.0f;
        const glm::vec3 to = position - anchor;
        const float length = glm::length(to);
        if (length > 0.0f) {
            if (const auto hit = m_physics->raycast(anchor * k_u, to / length, (length + radius) * k_u)) {
                position = anchor + to / length * std::max(hit->distance / k_u - radius, 0.0f);
            }
        }
    }
    m_camera.position = position * k_u;
    const glm::vec3 view = look - position;
    const float distance = glm::length(view);
    if (distance > 0.001f) {
        m_camera.yaw = std::atan2(view.x, view.y);
        m_camera.set_pitch(std::asin(std::clamp(view.z / distance, -1.0f, 1.0f)));
    }
    if (k.model->rotates) {
        const glm::vec3 e = mix(a.angles, b.angles);
        const glm::mat3 to_view(glm::vec3(1, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
        const glm::mat3 keys(glm::rotate(glm::rotate(glm::rotate(glm::mat4(1.0f), e.x, glm::vec3(1, 0, 0)), e.y, glm::vec3(0, 1, 0)), e.z, glm::vec3(0, 0, 1)));
        m_camera.sway = to_view * glm::transpose(keys) * glm::transpose(to_view);
    }
    const float right = glm::mix(a.right, b.right, t);
    m_camera.fov_y = 2.0f * std::atan((right > 0.0f ? right : k.model->frustum_right) * 0.75f);
}

// a fixed factor a frame towards the target. The game's frames are 60 a second
// here (measured: x 1.43 and x 0.69 over 18 frames), so the factor goes by that clock.
void App::step_kill_rate(float target, float seconds) {
    constexpr float k_rate_per_frame = 0.02f;
    float& rate = m_kill_camera.rate;
    const float frames = seconds * 60.0f;
    if (rate > target) {
        rate = std::max(rate * std::pow(1.0f - k_rate_per_frame, frames), target);
    } else if (rate < target) {
        rate = std::min(rate * std::pow(1.0f + k_rate_per_frame, frames), target);
    }
    m_time_rate = m_player_rate = rate;
}

// VATS_UpdateKillCam, on real seconds.
void App::update_kill_camera(float seconds) {
    KillCamera& k = m_kill_camera;
    if (!k.active) {
        return;
    }
    const game::Settings& settings = m_game->settings();
    game::Combat& combat = m_game->combat();
    ActorSet* actors = m_space ? m_space->actors() : nullptr;
    glm::vec3 feet;
    float heading = 0.0f;
    const bool paired = actors && (actors->paired(ActorSet::k_player_body, feet, heading) || m_kill_move.target);
    const bool dead = !k.target || combat.fighter(k.target).dead();
    const game::AttackState state = combat.fighter(game::k_player_ref).attack_state;
    // Phase: 2 once the blow has fallen, or the shot has landed; 1 while it flies.
    const game::Projectile* flying = k.projectile ? m_game->projectiles().find(k.projectile) : nullptr;
    const bool in_air = flying && !flying->dead && flying->flight == game::ArrowFlight::flying && flying->impacts.empty();
    if (k.ranged) {
        if (k.phase == 0 && k.projectile) {
            k.phase = 1;
        }
        if (dead) {
            k.phase = 2; // a stream or an area doesn't land
        }
        game::CombatBody body;
        const bool close = in_air && combat.world().body(k.target, body) &&
                           glm::distance(glm::vec3(flying->position[0], flying->position[1], flying->position[2]),
                                         glm::vec3(body.position[0], body.position[1], body.position[2] + body.height * 0.5f)) <=
                               settings.number("iVATSCameraHitDist", 100.0f) + body.radius;
        if (k.phase == 1 && (!in_air || close)) {
            k.phase = 2;
        }
    } else if (k.phase == 0 && ((state != game::AttackState::draw && state != game::AttackState::swing) || dead)) {
        k.phase = 2;
    }
    k.elapsed += seconds;
    k.total += seconds;
    const auto shots = [&]() -> const std::vector<game::CameraShot>& {
        static const std::vector<game::CameraShot> none;
        return k.path ? k.path->shots : none;
    };
    const auto current = [&]() -> const game::CameraShot* { return k.shot < shots().size() ? &shots()[k.shot] : nullptr; };
    if (const game::CameraShot* s = current(); s && k.running) {
        // Once min is over and the blow has happened: a Shoot shot waits for the blow, else runs to max (4, derived).
        k.min = std::max(k.min - seconds, 0.0f);
        bool advance = k.min <= 0.0f && k.phase > s->action;
        if (k.max > 0.0f) {
            k.max -= seconds;
            advance = advance || k.max <= 0.0f;
        }
        // Near target: a tracked projectile close to where it's aimed ends the shot as max would, once.
        if (in_air && s->near_distance > 0.0f && !k.near_done) {
            game::CombatBody body;
            if (combat.world().body(k.target, body) &&
                glm::distance(glm::vec3(flying->position[0], flying->position[1], flying->position[2]),
                              glm::vec3(body.position[0], body.position[1], body.position[2] + body.height * 0.5f)) < s->near_distance) {
                k.near_done = true;
                advance = true;
            }
        }
        if (advance) {
            k.min = std::max(k.min, 0.0f);
            next_kill_shot();
        }
    }
    if (const game::CameraShot* s = current(); s && !k.running && (k.phase >= s->action || s->action == game::CameraShot::zoom)) {
        begin_kill_shot(*s);
    }
    // Limits (8): a kill move that broke up ends it; so does the time.
    const float cap = current() ? k.max : settings.number("fVATSKillMoveEnd", 4.0f);
    if (!k.ranged && k.limit - k.total > cap && !paired && !dead) {
        k.total = k.limit + 1.0f;
    }
    const bool finished = k.ranged ? k.phase == 2 && !in_air && (!current() || k.min <= 0.0f) : !paired && (!current() || k.min <= 0.0f);
    if (finished || k.total > k.limit) {
        if (!k.ending) {
            k.ending = true;
            k.delay = settings.number("fVATSPlaybackDelay", 0.17f);
        }
        k.delay -= seconds;
        if (k.delay <= 0.0f) {
            end_kill_camera();
            return;
        }
    }
    const game::CameraShot* s = current();
    if (s && k.running) {
        // 7: a rate at or below 0 means the settings'.
        const float player = s->player_rate > 0.0f ? s->player_rate : settings.number("fVATSPlayerTimeUpdateMult", 6.0f);
        const float others = s->target_rate > 0.0f ? s->target_rate : settings.number("fVATSTargetTimeUpdateMult", 1.0f);
        step_kill_rate(s->global_rate, seconds);
        m_game->projectiles().player_rate = player;
        if (actors) {
            actors->set_graph_rates(player, others);
        }
        if (!(s->flags & game::CameraShot::first_person)) {
            place_kill_camera(*s);
            k.last_position = m_camera.position;
            k.last_yaw = m_camera.yaw;
            k.last_pitch = m_camera.pitch;
            k.last_sway = m_camera.sway;
            k.placed = true;
        }
    } else {
        step_kill_rate(1.0f, seconds);
        m_game->projectiles().player_rate = 1.0f;
        if (actors) {
            actors->set_graph_rates(1.0f, 1.0f);
        }
        if (k.placed) { // between shots and after the last, the view stays where it was
            m_camera.position = k.last_position;
            m_camera.yaw = k.last_yaw;
            m_camera.set_pitch(k.last_pitch);
            m_camera.sway = k.last_sway;
        }
    }
}

} // namespace spud::engine
