// The actors' AI in the frame: game::Ai decides where everyone is and walks those near the player; the spaces' ActorSets
// show them.

#include "core/trace.hpp"
#include "engine/app_internal.hpp"

#include "core/game_random.hpp"
#include "core/log.hpp"
#include "core/units.hpp"
#include "game/scenes.hpp"
#include "game/story_manager.hpp"
#include "game/detection.hpp"
#include "game/dialogue.hpp"
#include "game/game.hpp"

#include <algorithm>
#include <chrono>

namespace spud::engine {

namespace {
constexpr uint32_t k_player_npc = 0x7;  // the Player form
constexpr uint32_t k_race_child = 0x4; // RACE DATA
} // namespace

namespace {

constexpr uint32_t k_crime_forcegreet = 0x000267DE; // DGCrimeForcegreetTopic

std::vector<uint32_t> space_ids(const Space& space) {
    const osw::World& w = space.world();
    std::vector<uint32_t> ids;
    if (space.interior()) {
        if (!w.cells.empty()) {
            ids.push_back(w.cells[0].form_id);
        }
        return ids;
    }
    ids.push_back(w.form_id);
    for (const auto& region : w.regions) {
        ids.push_back(region.form_id);
    }
    return ids;
}

} // namespace

// What sandboxing actors can use in the player's space, read from its references when it changes.
void App::offer_furniture() {
    if (!m_space || m_ai_furniture_space == m_space.get()) {
        return;
    }
    m_ai_furniture_space = m_space.get();
    for (const osw::Ref& r : m_space->world().refs) {
        if (m_game->harvested(r.form_id)) {
            show_harvested(r);
        }
    }
    std::vector<game::AiFurniture> items;
    // From the records, not the world's references: those are what draws, and a lean or idle marker doesn't.
    for (const uint32_t id : space_ids(*m_space)) {
        for (const PlacedMarker& r : m_actor_data->usable_markers(id)) {
            const game::FurnitureKind kind = m_game->furniture_kind(r.base);
            if (kind == game::FurnitureKind::none) {
                continue;
            }
            const size_t before = items.size();
            const auto first = m_rules ? std::ranges::lower_bound(m_rules->furniture_markers, r.base, {}, &osr::FurnitureMarker::form)
                                       : std::vector<osr::FurnitureMarker>::const_iterator{};
            if (m_rules) {
                const uint32_t active = m_game->furniture_active_markers(r.base);
                for (auto m = first; m != m_rules->furniture_markers.end() && m->form == r.base; ++m) {
                    if (m->index >= 24 || !(active & (1u << m->index))) {
                        continue;
                    }
                    // The marker's place turns and scales with the reference; heading runs clockwise from north.
                    const float s = std::sin(r.heading), c = std::cos(r.heading);
                    const float x = (m->offset[0] * c + m->offset[1] * s) * r.scale, y = (-m->offset[0] * s + m->offset[1] * c) * r.scale;
                    // Sit idles are authored on a 34 unit seat from the floor (measured: at the marker's full height an
                    // actor floats on CommonChair01, whose marker is 34.0 up), so a stool lifts its user by the difference. Sleep
                    // idles float the same way: their marker's height isn't added.
                    const bool seat = kind == game::FurnitureKind::chair && m_game->furniture_events(r.base).enter.empty();
                    const float lift = seat ? (m->offset[2] - 34.0f) * r.scale : 0.0f;
                    items.push_back({r.ref, r.base, kind, {r.position.x + x, r.position.y + y, r.position.z + lift}, r.heading + m->heading,
                                     uint8_t(m->index), m_game->furniture_entry_points(r.base, m->index)});
                }
            }
            if (items.size() == before) {
                items.push_back({r.ref, r.base, kind, {r.position.x, r.position.y, r.position.z}, r.heading});
            }
        }
    }
    log::info("ai: {} things to use in {}", items.size(), m_space->edid());
    std::vector<game::AiLanding> landings;
    for (const osw::Ref& r : m_space->world().refs) {
        // Dragon Land and Crash Zone Markers (default objects 29, 30).
        if (r.base_form_id == 0x000138C0 || r.base_form_id == 0x0003DF55) {
            const glm::vec3 p = glm::vec3(r.position[0], r.position[1], r.position[2]) / units::k_skyrim_unit_m;
            landings.push_back({{p.x, p.y, p.z}, r.base_form_id == 0x0003DF55});
        }
    }
    const std::vector<uint32_t> ids = space_ids(*m_space);
    for (const uint32_t id : ids) {
        m_game->ai().set_furniture(id, items);
        m_game->ai().set_landings(id, landings);
    }
}

// Who is in a space, for its ActorSet.
uint32_t App::player_space() const {
    if (!m_space) {
        return 0;
    }
    const std::vector<uint32_t> ids = space_ids(*m_space);
    return ids.empty() ? 0 : ids[0];
}

// 0 Loud, 1 Normal, 2 Silent, 3 Very Loud.
void App::player_noise(int level) {
    const game::Settings& s = m_game->settings();
    constexpr std::array<std::pair<const char*, float>, 4> k_levels = {
        {{"iSoundLevelLoud", 100.0f}, {"iSoundLevelNormal", 50.0f}, {"iSoundLevelSilent", 10.0f}, {"iSoundLevelVeryLoud", 200.0f}}};
    const auto& [name, fallback] = k_levels[size_t(std::clamp(level, 0, 3))];
    m_action_sound = int(s.number(name, fallback));
    m_action_sound_left = s.number("fActorAlertSoundTimer", 5.0f);
}

// A merged city's actors stand in the world around it: the same ground to the player and to a fight.
uint32_t App::outer_space(uint32_t space) const {
    if (m_space && !m_space->interior()) {
        const osw::World& w = m_space->world();
        for (const auto& region : w.regions) {
            if (region.form_id == space) {
                return w.form_id;
            }
        }
    }
    return space;
}

void App::apply_roster(Space& space) {
    ActorSet* actors = space.actors();
    if (!actors) {
        return;
    }
    std::vector<PlacedActor> placed;
    for (const uint32_t id : space_ids(space)) {
        for (const game::Agent* a : m_game->ai().in_space(id)) {
            if (m_game->ref_disabled(a->ref)) {
                continue;
            }
            PlacedActor p;
            p.ref = a->ref;
            p.base = a->npc;
            p.position = glm::vec3(a->position[0], a->position[1], a->position[2]) * units::k_skyrim_unit_m;
            p.heading = a->heading;
            p.scale = a->scale;
            placed.push_back(p);
        }
    }
    actors->set_placed(std::move(placed));
}

// The light on the player, each fLightRecalcTimerPlayer seconds: the lights
// reaching the torso, the ambient, and in the open the sun when a ray to it is clear.
float App::player_light() {
    if (!m_space || !m_player) {
        return 0;
    }
    const render::Environment& env = m_space->environment();
    const glm::vec3 torso = m_player->feet() + glm::vec3(0, 0, 64.0f * units::k_skyrim_unit_m);
    float sum = 0;
    for (const auto& l : m_space->world().lights) {
        const glm::vec3 d = glm::vec3(l.position[0], l.position[1], l.position[2]) - torso;
        if (l.radius <= 0.0f || glm::dot(d, d) >= l.radius * l.radius) {
            continue;
        }
        const float t = std::min(glm::length(d) / l.radius, 1.0f);
        sum += (l.color[0] + l.color[1] + l.color[2]) * l.dimmer / 3.0f * (1.0f - t * t);
    }
    const float ambient = (env.ambient_sky.r + env.ambient_sky.g + env.ambient_sky.b) / 3.0f;
    float level = sum + ambient;
    if (!m_space->interior()) {
        const auto hit = m_physics ? m_physics->raycast(torso, env.sun_dir, 4096.0f * units::k_skyrim_unit_m) : std::nullopt;
        if (!hit) {
            level += (env.sun_color.r + env.sun_color.g + env.sun_color.b) / 3.0f;
        }
    }
    const game::Settings& s = m_game->settings();
    int value = int(100.0f * level);
    if (m_space->interior()) {
        value -= std::min(int(100.0f * std::max({env.ambient_sky.r, env.ambient_sky.g, env.ambient_sky.b})),
                          int(s.number("iLightLevelInteriorMod", 25)));
    } else {
        value -= int(s.number("iLightLevelExteriorMod", 0));
    }
    return float(std::clamp(value, 0, int(s.number("iLightLevelMax", 300))));
}

// Each high actor's detection of the player, refreshed about once a second; the player's
// stealth eye while sneaking: out of combat, 100 when anyone sees them.
void App::update_detection(float dt) {
    if (!m_walk || !m_player || !m_space) {
        return;
    }
    const bool sneaking = m_player->status().sneaking;
    m_detection_timer -= dt;
    if (m_detection_timer > 0.0f) {
        return;
    }
    m_detection_timer = 1.0f;
    m_player_light = player_light();
    const game::Settings& s = m_game->settings();
    const glm::vec3 feet = m_player->feet() / units::k_skyrim_unit_m;
    const float speed = m_player->status().speed;
    float weight = 0;
    for (const game::Stack& st : m_game->player().inventory.stacks()) {
        const game::Item* i = st.worn != game::Worn::none ? m_game->items().find(st.form) : nullptr;
        weight += i ? i->weight : 0.0f;
    }
    int highest = -1000;
    ActorSet* actors = m_space->actors();
    if (!actors) {
        return;
    }
    game::Ai& ai = m_game->ai();
    const float eye = 96.0f, sneak = sneaking ? 0.57f : 1.0f;
    for (const ActorSet::Who& e : actors->who()) {
        game::Agent* a = ai.mutable_agent(e.ref);
        if (!a) {
            continue;
        }
        const glm::vec3 pos(a->position[0], a->position[1], a->position[2]);
        const glm::vec3 to = feet - pos;
        const float distance = glm::length(to);
        if (distance >= 8192.0f) {
            a->detection = -1000;
            a->los = false;
            continue;
        }
        const float bearing = std::atan2(to.x, to.y);
        float angle = std::abs(glm::degrees(bearing - a->heading));
        angle = std::fmod(angle, 360.0f);
        angle = angle > 180.0f ? 360.0f - angle : angle;
        game::DetectionInput in;
        in.distance = distance;
        in.angle = angle;
        in.exterior = !m_space->interior();
        // Rays from the eye to the torso, the head, the feet, the first clear wins.
        if (distance < game::detection_sight_range(s, in.exterior, angle) && m_physics) {
            for (const float height : {64.0f * sneak, 108.0f * sneak, 19.0f * sneak}) {
                const glm::vec3 from = (pos + glm::vec3(0, 0, eye)) * units::k_skyrim_unit_m;
                const glm::vec3 target = (feet + glm::vec3(0, 0, height)) * units::k_skyrim_unit_m;
                const float length = glm::length(target - from);
                if (!m_physics->raycast(from, (target - from) / length, length)) {
                    in.los = true;
                    break;
                }
            }
        }
        in.observer_skill = m_game->npc_skill(a->npc, game::sneak);
        in.target_skill = game::player_detection_skill(s, m_game->character().skill(game::sneak));
        in.target_skill = m_game->perk_value(57, in.target_skill);
        in.sneaking = sneaking;
        in.moving = speed >= 5.0f;
        const float walk = m_ai_walk_speed, run = m_ai_run_speed;
        in.running = speed >= 0.25f * walk + 0.75f * run;
        in.light = m_game->perk_value(47, m_player_light);
        in.worn_weight = weight;
        in.action_sound = m_action_sound_left > 0.0f ? m_action_sound : 0;
        a->detection = game::detection_level(s, in);
        // Stage B: at or below 0, the player's last detection event may still be heard: a level of 0 at the sound.
        const game::DetectionEvent& noise = m_game->player_detection_event();
        if (a->detection <= 0 && noise.left > 0.0f) {
            const float dx = a->position[0] - noise.position[0], dy = a->position[1] - noise.position[1], dz = a->position[2] - noise.position[2];
            if (game::event_heard(s, noise.level, std::sqrt(dx * dx + dy * dy + dz * dz), in.exterior, true, in.observer_asleep)) {
                a->detection = 0;
            }
        }
        a->los = in.los;
        highest = std::max(highest, a->detection);
        // The game: over the limit, a guard who sees the player comes for them.
        game::Crime& crime = m_game->crime();
        const uint32_t faction = crime.crime_faction_of(a->npc);
        if (a->detection > 0 && !a->arresting && faction && crime.is_guard(a->npc) && crime.arrest_on_sight(faction)) {
            ai.pursue(*a, true);
        }
        // step 2: a level of 0 is enough, what they heard counts.
        game::Fighter& f = m_game->combat().fighter(a->ref);
        const float apart = glm::length(glm::vec3(a->position[0], a->position[1], a->position[2]) - feet);
        if (a->detection >= 0 && !f.dead() && m_game->combat_ai().should_attack(a->ref, e.npc, game::k_player_ref, 7, apart)) {
            m_game->combat().start_combat(a->ref, game::k_player_ref);
        }
    }
    // The same among NPCs: detection isn't worked out for them, so sight within the combat range stands in.
    const auto& who = actors->who();
    for (const ActorSet::Who& e : who) {
        const game::Agent* a = ai.agent(e.ref);
        if (!a || m_game->combat().fighter(e.ref).dead()) {
            continue;
        }
        for (const ActorSet::Who& other : who) {
            const game::Agent* b = other.ref != e.ref ? ai.agent(other.ref) : nullptr;
            if (!b) {
                continue;
            }
            const float apart = glm::length(glm::vec3(a->position[0] - b->position[0], a->position[1] - b->position[1], a->position[2] - b->position[2]));
            if (apart < s.number("fCombatMaximumRange", 4096.0f) && m_game->combat_ai().should_attack(e.ref, e.npc, other.ref, other.npc, apart) &&
                m_combat_world->line_of_sight(e.ref, other.ref)) {
                m_game->combat().start_combat(e.ref, other.ref);
            }
        }
    }
    if (m_ui) {
        m_ui->set_stealth(sneaking, highest > 0 ? 100.0 : 0.0);
    }
    // Sneak practice: unseen with an observer in line of sight, moving.
    if (sneaking && speed >= 5.0f && highest < 0 && highest != -1000) {
        m_game->use_skill(game::sneak, s.number("fSkillUsageSneakPerSecond", 0.625f) * 1.0f);
    }
}

// The game: hellos in reach of the player while seen; idle chatter outside it.
// the player breathes through a sprint while there's stamina left.
void App::sprint_breath(float dt) {
    m_breath_wait = std::max(m_breath_wait - dt, 0.0f);
    game::Dialogue* dialogue = m_papyrus ? m_papyrus->dialogue() : nullptr;
    const PlayerStatus& p = m_player->status();
    if (!dialogue || !p.sprinting || p.stamina <= 0.0f || p.riding || m_breath_wait > 0.0f ||
        dialogue->is_saying_line(game::k_player_ref)) {
        return;
    }
    constexpr uint16_t k_subtype_enter_sprint_breath = 95;
    if (game::Info* line = dialogue->pick_by_subtype(game::category_misc, k_subtype_enter_sprint_breath, game::k_player_ref, 0);
        line && line->topic) {
        dialogue->say(game::k_player_ref, line->topic->form, 0, true);
        m_breath_wait = 0.73f; // measured: the game's lines start 0.70 to 0.73 s apart, whatever the file's length
    }
}

// the air as the graph hears it. Falling sends Action Fall every step, and coming down
// sets VelocityZ for Action Land's idles. the game's Action Jump is sent with the lift-off here.
void App::player_air_actions() {
    using State = physics::ControllerState;
    if (!m_game) {
        return;
    }
    const physics::CharacterState& c = m_player->state();
    const bool air = c.state == State::jumping || c.state == State::in_air;
    const bool was_air = m_air_state == State::jumping || m_air_state == State::in_air;
    const glm::vec3 velocity = c.requested_velocity / units::k_skyrim_unit_m;
    game::Fighter& f = m_game->combat().fighter(game::k_player_ref);
    f.velocity = {velocity.x, velocity.y, velocity.z};
    f.move_speed = m_player->status().speed;
    if (m_combat_world && !m_mount.ref) {
        if (c.state == State::jumping && !was_air) {
            m_combat_world->perform_action(game::k_player_ref, "ActionJump");
        } else if (was_air && air && velocity.z <= 0.0f) {
            m_combat_world->perform_action(game::k_player_ref, "ActionFall");
        } else if (was_air && !air) {
            m_combat_world->graph_float(game::k_player_ref, "VelocityZ", m_air_velocity_z);
            m_combat_world->perform_action(game::k_player_ref, "ActionLand");
        }
    }
    m_air_state = c.state;
    m_air_velocity_z = velocity.z;
}

void App::update_greetings(float dt) {
    game::Dialogue* dialogue = m_papyrus ? m_papyrus->dialogue() : nullptr;
    ActorSet* actors = m_space ? m_space->actors() : nullptr;
    if (!dialogue || !actors || !m_player || !m_walk) {
        return;
    }
    const game::Settings& s = m_game->settings();
    game::Ai& ai = m_game->ai();
    const glm::vec3 feet = m_player->feet() / units::k_skyrim_unit_m;
    const float greet_range = s.number("fAIMinGreetingDistance", 150.0f);
    for (const ActorSet::Who& e : actors->who()) {
        game::Agent* a = ai.mutable_agent(e.ref);
        if (!a) {
            continue;
        }
        const bool talking = dialogue->is_saying_line(a->ref);
        a->saying = talking || dialogue->speaker_ref() == a->ref; // the conversation keeps them between lines
        if (dialogue->speaker_ref() == a->ref && a->use == game::UseState::none) {
            a->faces_player = true; // The game turns them as the game does
        }
        if (a->detection > 0 && !dialogue->in_conversation()) {
            a->greeting_timer -= dt; // runs down only while they see the player
        }
        if (talking || dialogue->in_conversation()) {
            continue;
        }
        const float d = glm::length(glm::vec3(a->position[0], a->position[1], a->position[2]) - feet);
        // In reach the guard opens the arrest (500 units); a ForceGreet package its conversation.
        const bool force = a->package && a->package->mode == game::AiMode::force_greet;
        if ((a->arresting && d <= 500.0f) || (force && d <= greet_range)) {
            game::Info* line = nullptr;
            if (a->arresting) {
                if (game::Topic* t = dialogue->topic(k_crime_forcegreet)) {
                    line = dialogue->pick_info(*t, a->ref, 0x14);
                }
                ai.pursue(*a, false);
            } else if (game::Topic* t = a->package->topic ? dialogue->topic(a->package->topic) : nullptr) {
                line = dialogue->pick_info(*t, a->ref, game::k_player_ref); // the package's own topic, if it has a line for them
            }
            dialogue->start_conversation(a->ref, line);
            continue;
        }
        if (a->detection > 0 && a->los && d <= greet_range && a->greeting_timer <= 0.0f) {
            if (game::Info* hello = dialogue->pick_greeting(a->ref); hello && hello->topic) {
                dialogue->say(a->ref, hello->topic->form, 0x14);
                a->greeting_timer = s.number("fAIGreetingTimer", 5.0f);
                a->faces_player = a->use == game::UseState::none; // stops and turns; not on a seat
            }
            continue;
        }
        // the running package has to allow idle chatter (interrupt bit 7).
        if (a->package && !(a->package->interrupts & 0x80)) {
            continue;
        }
        a->chatter_timer -= dt;
        if (d > greet_range && a->chatter_timer <= 0.0f) {
            const float low = s.number("fIdleChatterCommentTimer", 10.0f), high = s.number("fIdleChatterCommentTimerMax", 20.0f);
            a->chatter_timer = low + (high - low) * float(GameRandom::shared().next_unit());
            if (game::Info* line = dialogue->pick_by_subtype(game::category_misc, 94, a->ref, 0x14); line && line->topic) {
                dialogue->say(a->ref, line->topic->form, 0x14);
            }
        }
    }
}

// What shows on the player is what they have equipped.
void App::update_player_worn() {
    ActorSet* actors = m_space ? m_space->actors() : nullptr;
    if (!actors || !m_actor_data) {
        return;
    }
    std::vector<uint32_t> worn;
    uint32_t right = 0, left = 0;
    for (const game::Stack& s : m_game->player().inventory.stacks()) {
        if (s.worn != game::Worn::none) {
            worn.push_back(s.form);
            if (s.worn == game::Worn::right || s.worn == game::Worn::both) {
                right = s.form;
            }
            if (s.worn == game::Worn::left || s.worn == game::Worn::both) {
                left = s.form;
            }
        }
    }
    std::sort(worn.begin(), worn.end());
    const game::Character& character = m_game->character();
    const uint32_t beast = character.race() != character.original_race() ? character.race() : 0;
    if (beast != m_player_beast) {
        m_player_beast = beast;
        m_actor_data->set_player_beast(beast);
        m_player_dressed = false;
    }
    if (beast) {
        worn.clear(); // the form shows its own body; what's worn waits for it to end
        right = left = 0;
    }
    if (m_player) {
        m_player->set_form_camera_height(beast ? actors->node_offset(ActorSet::k_player_body, "camera3rd [cam3]").z : 0.0f);
    }
    if (worn == m_player_worn && right == m_player_right && left == m_player_left && m_player_dressed) {
        return;
    }
    m_player_worn = worn;
    m_player_right = right;
    m_player_left = left;
    m_player_dressed = true;
    m_actor_data->set_player_worn(std::move(worn), right, left);
    actors->rebuild_player();
    // A new body starts with its weapon away; the game keeps a drawn weapon out through a change of gear.
    m_redraw_weapon = m_game->combat().fighter(game::k_player_ref).weapon_drawn;
}

bool App::can_head_track(uint32_t ref, const game::Agent& a, uint32_t target, bool current, bool large) {
    game::Ai& ai = m_game->ai();
    const game::Agent* other = target == game::k_player_ref ? nullptr : ai.agent(target);
    const auto at = m_game->ref_position(target);
    if (target == ref || !at || (other && other->disabled) || m_game->combat().in_combat_with(ref, target)) {
        return false;
    }
    const float dx = (*at)[0] - a.position[0], dy = (*at)[1] - a.position[1], dz = (*at)[2] - a.position[2];
    const float squared = dx * dx + dy * dy + dz * dz;
    const float reach = large ? ai.setting("fAIMaxLargeCreatureHeadTrackDistance", 12000.0f)
                              : ai.setting("fAIMaxHeadTrackDistance", 400.0f);
    if (squared > reach * reach) {
        return false;
    }
    const bool commanded = a.commander != 0 || (other && other->commander != 0);
    if (squared < 10000.0f && !commanded && other && !current) {
        return false;
    }
    float angle = std::atan2(dx, dy) - a.heading;
    angle = std::remainder(angle, 2.0f * glm::pi<float>());
    if (std::abs(angle) > (current ? glm::pi<float>() : 2.0943952f) && !commanded) {
        return false;
    }
    if (!other) { // is the LOS flag, LOS within half the view cone
        const float cone = glm::radians(ai.setting("fDetectionViewCone", 190.0f)) * 0.5f;
        return commanded || (a.detection > 0 && (!current || a.los) && (a.speed < 5.0f || (a.los && std::abs(angle) < cone)));
    }
    const auto seated = [](const game::Agent& g) { return g.use == game::UseState::using_it && g.activity <= game::Activity::sleep && g.activity != game::Activity::none; };
    if (commanded) {
        return true;
    }
    // NPCs have no detection of each other here: plain sight stands in for a level above 0.
    return !(seated(a) && seated(*other)) && m_combat_world && m_combat_world->line_of_sight(ref, target);
}

// The game's default slot, and the pick: the player if eligible, else the first
// eligible high actor. The score always comes to 0, so nobody beats the first.
void App::update_gazes(float dt, ActorSet& actors, const glm::vec3& eye, uint32_t speaker) {
    game::Ai& ai = m_game->ai();
    std::erase_if(m_gazes, [&](const auto& g) {
        if (std::ranges::find(m_high_actors, g.first) != m_high_actors.end()) {
            return false;
        }
        actors.set_look_target(g.first, nullptr);
        return true;
    });
    const float far_small = ai.setting("fAIMaxHeadTrackDistanceFromPC", 2000.0f);
    const float far_large = ai.setting("fAIMaxLargeCreatureHeadTrackDistance", 12000.0f);
    const float hold = ai.setting("fAIHoldDefaultHeadTrackTimer", 5.0f);
    const auto player = m_game->ref_position(game::k_player_ref);
    int picks = 0;
    for (const uint32_t ref : m_high_actors) {
        const game::Agent* a = ai.agent(ref);
        if (!a || !player) {
            continue;
        }
        Gaze& g = m_gazes[ref];
        if (g.large < 0) {
            g.large = m_game->race_size(a->npc) == 3 ? 1 : 0;
        }
        const float far_from_player = g.large ? far_large : far_small;
        const float dx = (*player)[0] - a->position[0], dy = (*player)[1] - a->position[1], dz = (*player)[2] - a->position[2];
        const bool dead = m_game->combat().fighter(ref).dead();
        if (dead || dx * dx + dy * dy + dz * dz > far_from_player * far_from_player) {
            if (g.target) {
                actors.set_look_target(ref, nullptr);
                g.target = 0;
            }
            continue;
        }
        g.update -= dt;
        g.hold -= dt;
        const bool asked = ref == speaker || a->talk_partner || a->fighting || m_look_at.contains(ref);
        if (g.target == game::k_player_ref && a->detection <= 0 && !asked) {
            actors.set_look_target(ref, nullptr);
            g.target = 0;
        }
        constexpr int k_picks_per_frame = 10; // iUpdateActorsPerFrame:HeadTracking
        if (g.update <= 0.0f && picks < k_picks_per_frame) {
            ++picks;
            g.update = 1.0f + 0.5f * float(GameRandom::shared().next_unit()); // fUpdateDelaySecondsMin..Max:HeadTracking
            if (!asked && g.hold < 0.0f) {
                uint32_t pick = 0;
                if (can_head_track(ref, *a, game::k_player_ref, g.target == game::k_player_ref, g.large != 0)) {
                    pick = game::k_player_ref;
                }
                for (size_t i = 0; !pick && i < m_high_actors.size(); ++i) {
                    const uint32_t other = m_high_actors[i];
                    if (can_head_track(ref, *a, other, g.target == other, g.large != 0)) {
                        pick = other;
                    }
                }
                if (pick) {
                    g.target = pick;
                    g.hold = hold;
                }
            }
        }
        if (g.target && !asked) {
            if (const std::optional<glm::vec3> at = g.target == game::k_player_ref ? eye : actors.head_position(g.target)) {
                actors.set_look_target(ref, &*at);
            }
        } else if (!asked) {
            // No target: where the path goes. Two seconds on at this speed; a path too short for that is
            // looked along to its end, if that's at least 128 units away. At the height of the eyes.
            constexpr float k_look_seconds = 2.0f, k_least_units = 128.0f;
            float left = a->speed * k_look_seconds, walked = 0.0f;
            game::NavPoint from = a->position, ahead = from;
            bool found = false;
            for (size_t i = a->next; i < a->path.size() && left > 0.0f; ++i) {
                const game::NavPoint& to = a->path[i];
                const float x = to[0] - from[0], y = to[1] - from[1], z = to[2] - from[2];
                const float length = std::sqrt(x * x + y * y + z * z);
                const float t = length > left ? left / length : 1.0f;
                ahead = {from[0] + x * t, from[1] + y * t, from[2] + z * t};
                walked += length * t;
                left -= length * t;
                from = to;
                found = true;
            }
            if (found && a->speed >= 5.0f && (left <= 0.0f || walked >= k_least_units)) {
                if (g.update == 0.0f || g.eye_height == 0.0f) {
                    const std::optional<glm::vec3> head = actors.head_position(ref);
                    g.eye_height = head ? head->z / units::k_skyrim_unit_m - a->position[2] : 0.0f;
                }
                const glm::vec3 at = glm::vec3(ahead[0], ahead[1], ahead[2] + g.eye_height) * units::k_skyrim_unit_m;
                actors.set_look_target(ref, &at);
                g.along_path = true;
            } else if (g.along_path) {
                actors.set_look_target(ref, nullptr);
                g.along_path = false;
            }
        }
    }
}

// whoever talks to the player looks at them.
void App::update_head_tracking(float dt) {
    game::Dialogue* dialogue = m_papyrus ? m_papyrus->dialogue() : nullptr;
    ActorSet* actors = m_space ? m_space->actors() : nullptr;
    if (!actors || !m_player) {
        return;
    }
    const uint32_t speaker = dialogue && dialogue->in_conversation() ? dialogue->speaker_ref() : 0;
    if (m_head_tracker && m_head_tracker != speaker) {
        actors->set_look_target(m_head_tracker, nullptr);
    }
    m_head_tracker = speaker;
    const glm::vec3 eye = m_player->camera();
    if (speaker) {
        m_game->set_ref_flag(speaker, game::ref_talked_to_pc, true);
        actors->set_look_target(speaker, &eye);
    }
    // Two NPCs in a conversation look at each other.
    std::vector<uint32_t> talkers;
    for (const uint32_t actor : m_high_actors) {
        const game::Agent* a = m_game ? m_game->ai().agent(actor) : nullptr;
        if (a && a->talk_partner) {
            if (const std::optional<glm::vec3> at = actors->head_position(a->talk_partner)) {
                actors->set_look_target(actor, &*at);
                talkers.push_back(actor);
            }
        }
    }
    for (const uint32_t actor : m_talkers) {
        if (std::ranges::find(talkers, actor) == talkers.end()) {
            actors->set_look_target(actor, nullptr);
        }
    }
    m_talkers = std::move(talkers);
    update_gazes(dt, *actors, eye, speaker);
    // A script's target goes first in the game.
    for (const auto& [actor, target] : m_look_at) {
        if (const std::optional<glm::vec3> at = target == game::k_player_ref ? eye : actors->head_position(target)) {
            actors->set_look_target(actor, &*at);
        }
    }
}

void App::update_ai(float dt, bool early) {
    update_head_tracking(dt);
    m_action_sound_left = std::max(m_action_sound_left - dt, 0.0f);
    if (m_game) {
        m_game->set_player_riding(m_mount.ref && m_mount.stage == MountStage::riding);
        game::DetectionEvent& noise = m_game->player_detection_event();
        noise.left = std::max(noise.left - dt, 0.0f);
    }
    if (!m_game || !m_actor_data) {
        return;
    }
    update_player_worn();
    if (!m_ai_ready) {
        if (!m_ai_placing.valid()) {
            m_ai_placing = std::async(std::launch::async, [data = m_actor_data.get()] { return data->placed_everywhere(); });
        }
        if (m_ai_placing.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return;
        }
        game::Ai& ai = m_game->ai();
        for (const WorldActor& a : m_ai_placing.get()) {
            const glm::vec3 p = a.placed.position / units::k_skyrim_unit_m;
            ai.add(a.placed.ref, a.placed.base, a.space, {p.x, p.y, p.z}, a.placed.heading, a.placed.scale);
        }
        ai.apply_saved();
        log::info("ai: {} actors", ai.agents());
        m_ai_ready = true;
        m_ai_walk_speed = ai.walk_speed();
        m_ai_run_speed = ai.run_speed();
    }
    if (m_ui && m_ui->pausing_menus() > 0) {
        return;
    }
    game::AiClock clock;
    clock.hour = m_clock.hour();
    clock.time_scale = m_clock.time_scale();
    clock.month = int(m_clock.month());
    clock.date = int(m_clock.day());
    clock.day_of_week = int(m_clock.whole_days()) % 7; // the game opens on a Sundas
    std::vector<uint32_t> high;
    ActorSet* actors = m_space ? m_space->actors() : nullptr;
    if (actors) {
        for (const ActorSet::Who& e : actors->who()) {
            high.push_back(e.ref);
            if (game::Agent* a = m_game->ai().mutable_agent(e.ref); e.npc && a && !a->resolved) { // its base may have been a leveled list
                a->resolved = true;
                if (const auto radius = m_game->flight_radius(e.npc)) {
                    a->flier = true;
                    a->flight_radius = *radius * a->scale;
                }
            }
        }
    }
    game::Ai& ai = m_game->ai();
    // the hours an actor leaves high process, which its own respawn timer counts from.
    std::sort(high.begin(), high.end());
    for (const uint32_t ref : m_high_actors) {
        if (!std::binary_search(high.begin(), high.end(), ref)) {
            if (game::Agent* a = ai.mutable_agent(ref)) {
                a->detach_hours = m_clock.hours_passed();
            }
        }
    }
    m_high_actors = high;
    if (game::QuestSystem* quests = m_papyrus ? m_papyrus->quests() : nullptr) {
        quests->conversations = &ai;
    }
    if (!ai.ask_conversation) {
        ai.ask_conversation = [this](uint32_t a, uint32_t b) -> uint32_t {
            game::QuestSystem* quests = m_papyrus ? m_papyrus->quests() : nullptr;
            const game::Dialogue* dialogue = m_papyrus ? m_papyrus->dialogue() : nullptr;
            if (!quests || (dialogue && dialogue->in_conversation())) {
                return 0;
            }
            game::StoryEvent e;
            e.type = game::event_code("ADIA");
            e.add("L1", m_game->conditions().location_of(a));
            e.add("R1", a);
            e.add("R2", b);
            return quests->story().queue(e);
        };
        ai.in_scene = [this](uint32_t ref) {
            game::QuestSystem* quests = m_papyrus ? m_papyrus->quests() : nullptr;
            return quests && quests->scenes().has_actor(ref);
        };
        ai.line_of_sight = [this](uint32_t from, uint32_t to) { return m_combat_world && m_combat_world->line_of_sight(from, to); };
    }
    if (!ai.alias_packages) {
        // Asked each time: a load replaces the quest system.
        ai.alias_ref = [this](uint32_t quest, int32_t alias) -> uint32_t {
            game::Quest* q = m_papyrus ? m_papyrus->quests()->quest(quest) : nullptr;
            const auto it = q ? q->filled_refs.find(uint32_t(alias)) : decltype(q->filled_refs)::iterator{};
            return q && it != q->filled_refs.end() ? it->second : 0;
        };
        ai.alias_packages = [this](uint32_t ref) {
            game::QuestSystem* quests = m_papyrus ? m_papyrus->quests() : nullptr;
            return quests ? quests->alias_packages(ref) : std::vector<uint32_t>{};
        };
    }
    if (m_space) {
        const std::vector<uint32_t> ids = space_ids(*m_space);
        const glm::vec3 where = (m_player ? m_player->feet() : m_camera.position) / units::k_skyrim_unit_m;
        ai.set_player(ids.empty() ? 0 : ids[0], {where.x, where.y, where.z}, ids);
        ai.set_player_running(m_player && m_player->status().speed > ai.walk_speed() * 1.5f);
        // The attached cells' navmeshes, ahead of the actors who arrive in them: one cell further than they reach.
        const std::array<int32_t, 3> at = {int32_t(ids.empty() ? 0 : ids[0]), int32_t(std::floor(where.x / 4096.0f)),
                                           int32_t(std::floor(where.y / 4096.0f))};
        if (at != m_warmed_at && !ids.empty()) {
            m_warmed_at = at;
            m_game->navmeshes().warm(ids[0], {where.x, where.y, where.z}, m_grids_to_load / 2 + 1);
        }
    }
    offer_furniture();
    ai.ground_height = [this](float x, float y) {
        const float u = units::k_skyrim_unit_m;
        return m_player ? m_player->land_height(glm::vec2(x, y) * u) / u : std::numeric_limits<float>::quiet_NaN();
    };
    ai.set_replay(m_tour);
    // On play's clock the thinking and walking come half a tick early, on a frame between (run_frame): they only touch the
    // agents, and what the actors take from them still waits for the tick.
    if (!m_ai_stepped) {
        SPUD_TRACE("ai step");
        ai.update(dt, clock, high);
    }
    if (early) {
        m_ai_stepped = true;
        return;
    }
    m_ai_stepped = false;
    // The whole space's list again for every actor who changes place: on play's clock, an eighth of a second's worth at once.
    constexpr uint32_t k_roster_ticks = 8;
    m_roster_wait += m_roster_wait < k_roster_ticks ? 1u : 0u;
    if (ai.roster() != m_ai_roster && (m_fixed_step <= 0.0 || m_roster_wait >= k_roster_ticks)) {
        SPUD_TRACE("roster");
        m_roster_wait = 0;
        m_ai_roster = ai.roster();
        if (m_space) {
            apply_roster(*m_space);
        }
        for (auto& o : m_others) {
            if (o->space) {
                apply_roster(*o->space);
            }
        }
    }
    if (m_world_still) {
        return;
    }
    {
        SPUD_TRACE("detection");
        update_detection(dt);
    }
    {
        SPUD_TRACE("greetings");
        update_greetings(dt);
    }
    if (actors) {
        for (const uint32_t ref : high) {
            if (const game::Agent* a = ai.agent(ref)) {
                glm::vec3 p = glm::vec3(a->position[0], a->position[1], a->position[2]) * units::k_skyrim_unit_m;
                if (ref == m_mount.ref && !m_mount.dragon) {
                    continue; // the player's character moves it
                }
                p += npc_rides(*a, *actors, p);
                const bool flying = a->fly != game::FlyState::none && a->fly != game::FlyState::perching;
                actors->move(ref, p, a->heading, flying || a->next < a->path.size() ? a->speed : 0.0f, glm::degrees(a->pitch));
                if (a->fly == game::FlyState::take_off || a->fly == game::FlyState::landing) {
                    // These two the animation flies: its root motion is the actor's.
                    const glm::vec3 t = actors->take_motion(ref);
                    const float c = std::cos(a->heading), s = std::sin(a->heading);
                    game::Agent* m = ai.mutable_agent(ref);
                    m->position[0] += t.x * c + t.y * s;
                    m->position[1] += -t.x * s + t.y * c;
                    m->position[2] += t.z;
                }
                if (!a->idle.empty() && actors->play_idle(ref, a->idle)) {
                    ai.mutable_agent(ref)->idle.clear();
                }
                if (!a->event.empty() && actors->send_event(ref, a->event)) {
                    ai.mutable_agent(ref)->event.clear();
                }
                if (!a->action.empty()) {
                    // An action is performed once, its event the idle tree's for it at what it's for: refused, it fails.
                    // Furniture whose Exit idle spud's conditions don't reach yet gets what the Exit idles send: a
                    // bed's IdleBedExitStart, everything else's IdleChairExitStart (the IDLE records).
                    game::Agent* m = ai.mutable_agent(ref);
                    std::string event = m_game->idles().pick(m->action, "actors\\character", ref, m->action_target);
                    if (event.empty() && m->action == "ActionActivate") {
                        event = m->activity == game::Activity::sleep ? "IdleBedExitStart" : "IdleChairExitStart";
                    }
                    if (!event.empty()) {
                        actors->send_event(ref, event);
                    }
                    m->action.clear();
                }
            }
        }
    }
}

// The actors near a walking player stop it. Its own mount and a partner in a
// paired animation don't: the game sets those down together.
void App::update_actor_bodies(ActorSet& actors) {
    if (!m_physics) {
        return;
    }
    m_actor_bodies.clear();
    if (m_player && m_walk && m_game) {
        constexpr float k_reach_m = 24.0f;
        const glm::vec3 feet = m_player->feet();
        actors.solids(m_solids);
        for (const ActorSet::Solid& s : m_solids) {
            glm::vec3 unused(0.0f);
            float turn = 0.0f;
            if (s.npc == k_player_npc || s.ref == m_mount.ref || glm::distance(s.feet, feet) > k_reach_m ||
                actors.paired(s.ref, unused, turn)) {
                continue;
            }
            // As the game: a humanoid's controller is its skeleton's bound as it stands, so
            // a tall Nord is no wider; a child's only grows upward, a creature's all ways by its scale.
            const osr::Race* race = m_rules ? m_rules->race(s.race) : nullptr;
            const auto is_npc = m_race_is_npc.try_emplace(s.race, false);
            if (is_npc.second) {
                is_npc.first->second = m_game->race_has_keyword(s.race, "ActorTypeNPC");
            }
            const bool child = race && (race->flags & k_race_child);
            const float scale = !is_npc.first->second || child ? s.scale : 1.0f;
            const int sex = s.female ? 1 : 0;
            //'s fallback for a skeleton with no bound.
            glm::vec3 half = glm::vec3(23.0f, 17.5f, 64.0f) * units::k_skyrim_unit_m;
            glm::vec2 centre(0.0f);
            if (race && race->bound_half[sex][2] > 0.0f) {
                half = glm::vec3(race->bound_half[sex][0], race->bound_half[sex][1], race->bound_half[sex][2]);
                centre = glm::vec2(race->bound_centre[sex][0], race->bound_centre[sex][1]);
            }
            const float across = child ? 1.0f : scale;
            m_actor_bodies.push_back({s.ref, s.feet, s.heading, half * glm::vec3(across, across, scale), centre * scale, scale});
        }
        std::ranges::sort(m_actor_bodies, {}, [&](const physics::World::ActorBody& a) { return glm::distance(a.feet, feet); });
    }
    m_physics->set_actor_bodies(m_actor_bodies);
}

} // namespace spud::engine
