// The world side of combat: bodies, graphs, the player's swing.

#include "core/log.hpp"
#include "app_internal.hpp"

#include "core/units.hpp"
#include "keys.hpp"
#include "game/story_manager.hpp"
#include "game/game.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <numbers>

namespace spud::engine {

constexpr float k_default_radius = 16.0f; // an actor with empty bounds
constexpr uint8_t k_layer_weapon = 5;     // L_WEAPON: what a swing's ray collides with

class EngineCombat final : public game::CombatWorld {
public:
    explicit EngineCombat(App& app) : m_app(app) {}

    ActorSet* actors() { return m_app.m_space ? m_app.m_space->actors() : nullptr; }
    static uint32_t graph_ref(uint32_t ref) { return ref == game::k_player_ref ? ActorSet::k_player_body : ref; }

    void push(uint32_t ref, const game::Units3& direction, float force, float seconds) override {
        if (ref != game::k_player_ref || !m_app.m_player) {
            return; // NPCs have no character controller here
        }
        const glm::vec3 d(direction[0], direction[1], direction[2]);
        const float length = glm::length(d);
        if (length > 0.0f) {
            m_app.m_player->knock(d / length * (force * units::k_skyrim_unit_m), seconds);
        }
    }

    bool in_interior() override { return m_app.m_space && (m_app.m_space->world().flags & osw::world_interior); }

    bool body(uint32_t ref, game::CombatBody& out) override {
        game::Game& g = *m_app.m_game;
        float scale = 1.0f;
        uint32_t npc = game::k_player_base;
        if (ref == game::k_player_ref) {
            if (!m_app.m_player) {
                return false;
            }
            const glm::vec3 p = m_app.m_player->feet() / units::k_skyrim_unit_m;
            out.position = {p.x, p.y, p.z};
            out.heading = m_app.m_camera.skyrim_heading_deg();
            out.space = m_app.player_space();
        } else {
            const game::Agent* a = g.ai().agent(ref);
            if (!a) {
                return false;
            }
            out.position = {a->position[0], a->position[1], a->position[2]};
            out.heading = a->heading * 180.0f / std::numbers::pi_v<float>;
            out.space = m_app.outer_space(a->space);
            out.loaded = a->high;
            scale = a->scale;
            npc = a->npc;
        }
        const Bounds& b = bounds(npc);
        out.scale = scale;
        out.full_scale = scale * b.height_scale;
        out.radius = b.y2 > b.y1 ? float(b.y2) * out.full_scale : k_default_radius;
        out.height = float(b.z2 - b.z1) * out.full_scale;
        return true;
    }

    std::vector<uint32_t> high_actors() override {
        std::vector<uint32_t> out;
        if (ActorSet* a = actors()) {
            for (const ActorSet::Entry& e : a->list()) {
                out.push_back(e.ref);
            }
        }
        return out;
    }

    bool graph_bool(uint32_t ref, std::string_view name) override {
        ActorSet* a = actors();
        return a && a->get_bool(graph_ref(ref), name);
    }
    void graph_int(uint32_t ref, std::string_view name, int value) override {
        if (ActorSet* a = actors()) {
            a->set_variable(graph_ref(ref), name, float(value));
        }
    }
    bool graph_value(uint32_t ref, std::string_view name, int32_t& out) override {
        ActorSet* a = actors();
        return a && a->get_int(graph_ref(ref), name, out);
    }
    bool graph_real(uint32_t ref, std::string_view name, float& out) override {
        ActorSet* a = actors();
        return a && a->get_float(graph_ref(ref), name, out);
    }
    bool queue_action(uint32_t ref, std::string_view action) override {
        ActorSet* a = actors();
        if (!a) {
            return false;
        }
        const uint32_t graph = graph_ref(ref);
        const uint32_t target = m_app.m_game->combat().fighter(ref).combat_target;
        std::string event = m_app.m_game->idles().pick(action, a->behaviour_folder(graph), ref, target);
        if (event.empty()) {
            return false;
        }
        a->queue_event(graph, std::move(event));
        return true;
    }
    void clear_actions(uint32_t ref) override {
        if (ActorSet* a = actors()) {
            a->clear_events(graph_ref(ref));
        }
    }
    bool want_cast(uint32_t ref, std::string_view name, bool on) override {
        ActorSet* a = actors();
        return a && a->set_variable(graph_ref(ref), name, on ? 1.0f : 0.0f);
    }
    void graph_float(uint32_t ref, std::string_view name, float value) override {
        if (ActorSet* a = actors()) {
            a->set_variable(graph_ref(ref), name, value);
        }
    }
    bool graph_event(uint32_t ref, std::string_view event) override {
        ActorSet* a = actors();
        return a && a->send_event(graph_ref(ref), event);
    }
    bool perform_action(uint32_t ref, std::string_view action) override {
        ActorSet* a = actors();
        if (!a) {
            return false;
        }
        const uint32_t graph = graph_ref(ref);
        game::Combat& combat = m_app.m_game->combat();
        uint32_t target = combat.fighter(ref).combat_target;
        if (!target && ref == game::k_player_ref && action.find("Attack") != std::string_view::npos) {
            target = combat.melee_target(ref); // the player has no combat target of its own
        }
        const std::string folder = a->behaviour_folder(graph);
        std::string event = m_app.m_game->idles().pick(action, folder, ref, target);
        if (event.empty()) {
            return false;
        }
        const bool kill = target && is_kill_move(event);
        const bool paired = kill && start_kill_move(*a, ref, target, event);
        if (kill && !paired) {
            event = m_app.m_game->idles().pick(action, folder, ref, target, false);
        }
        if (!paired && (event.empty() || !a->send_event(graph, event))) {
            return false;
        }
        combat.attack_started(ref, event, target);
        return true;
    }
    bool kill_move(uint32_t ref, std::string_view action) override {
        ActorSet* a = actors();
        game::Combat& combat = m_app.m_game->combat();
        const uint32_t target = combat.fighter(ref).combat_target;
        // Still drawing, its graph takes no pa_ event, and the victim would wait in the scene alone.
        if (!a || !target || combat.fighter(target).in_kill_move || !a->weapon_drawn(graph_ref(ref))) {
            return false;
        }
        const std::string event = m_app.m_game->idles().pick(action, a->behaviour_folder(graph_ref(ref)), ref, target);
        if (!is_kill_move(event) || !start_kill_move(*a, ref, target, event)) {
            return false;
        }
        combat.attack_started(ref, event, target);
        return true;
    }
    static bool is_kill_move(std::string_view event) {
        return event.size() > 7 && std::equal(event.begin(), event.begin() + 7, "pa_Kill", [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
        });
    }
    // pa_X for the one doing it, X for the victim, in one scene.
    bool start_kill_move(ActorSet& a, uint32_t ref, uint32_t target, const std::string& event) {
        game::Ai& ai = m_app.m_game->ai();
        const bool player = ref == game::k_player_ref || target == game::k_player_ref;
        // The player's own, and only that, comes with the kill camera; its first shot can keep first person.
        const game::CameraPath* path = nullptr;
        if (ref == game::k_player_ref) {
            const game::Gear gear = m_app.m_game->combat().gear(ref);
            const uint8_t type = gear.right.item ? gear.right.item->animation_type : 0;
            path = m_app.pick_kill_camera(target, type == 0 ? 0 : type == 5 || type == 6 ? 2 : 1);
        }
        const bool stays_first = path && !path->shots.empty() && path->shots.front().flags & game::CameraShot::first_person;
        if (player && !stays_first && m_app.m_player && m_app.m_player->view() == View::first_person) {
            // The third-person body plays it, once it's up: the game runs both graphs all along.
            m_app.m_kill_move = {ref, target, event, 0.0f};
            m_app.m_kill_move_view = true;
            m_app.m_player->set_view(View::third_person);
        } else if (!a.start_pair(graph_ref(ref), graph_ref(target), event, std::string_view(event).substr(3))) {
            return false;
        }
        if (ref == game::k_player_ref) {
            m_app.start_kill_camera(target, path);
        }
        for (const uint32_t side : {ref, target}) {
            if (side != game::k_player_ref) {
                ai.hold(side, true);
                m_app.m_paired.push_back(side);
            }
        }
        return true;
    }
    std::vector<uint32_t> weapons(uint32_t ref) override {
        ActorSet* a = actors();
        return a ? a->weapons(ref) : std::vector<uint32_t>{};
    }
    void decapitate(uint32_t ref) override {
        ActorSet* a = actors();
        if (m_app.m_actor_data) {
            m_app.m_actor_data->set_dismembered_limb_velocity(m_app.m_game->settings().number("fCombatDismemberedLimbVelocity", 10.0f));
        }
        if (!a || !a->decapitate(ref == game::k_player_ref ? ActorSet::k_player_body : ref, true)) {
            return;
        }
        if (const uint32_t art = a->decapitate_art(ref == game::k_player_ref ? ActorSet::k_player_body : ref)) {
            RefEffects::Hit hit;
            hit.id = 0xD0000000u | (ref & 0x0FFFFFFFu); // its own, past any magic effect's
            hit.target = ref;
            hit.art = art;
            hit.persist = true; // runs its time, not one shot
            hit.lifetime = m_app.m_game->settings().number("fDecapitateBloodTime", 5.0f);
            hit.node = "npc neck [neck]";
            m_app.ref_effects().start(hit);
        }
    }
    bool limb_gone(uint32_t ref, uint32_t part) override {
        return part == 1 && m_app.m_actor_data && m_app.m_actor_data->npc_is_headless(ref);
    }
    void swap_weapon(uint32_t ref, uint32_t form) override {
        if (ActorSet* a = actors()) {
            a->swap_weapon(ref, form);
        }
    }
    void play_at(uint32_t ref, uint32_t sound) override {
        game::CombatBody at;
        const auto descriptor = m_app.m_audio ? m_app.m_audio->find_descriptor(std::format("{:08X}", sound)) : std::nullopt;
        if (descriptor && body(ref, at)) {
            m_app.m_audio->play(*descriptor, glm::vec3(at.position[0], at.position[1], at.position[2]) * units::k_skyrim_unit_m, m_app.body_sound_model(ref));
        }
    }
    bool moving(uint32_t ref, float& direction) override {
        if (ref != game::k_player_ref || !m_app.m_player) {
            return false;
        }
        const glm::vec3 v = m_app.m_player->state().velocity / units::k_skyrim_unit_m;
        if (std::hypot(v.x, v.y) < 5.0f) {
            return false;
        }
        const float yaw = m_app.m_player->yaw();
        direction = std::atan2(v.x * std::cos(yaw) - v.y * std::sin(yaw), v.x * std::sin(yaw) + v.y * std::cos(yaw));
        return true;
    }
    void action_sound(uint32_t ref, int level) override {
        if (ref == game::k_player_ref) {
            m_app.player_noise(level);
        }
    }
    bool weapon_in_hand(uint32_t ref) override {
        ActorSet* a = actors();
        return a && a->weapon_drawn(ref);
    }
    void gear(uint32_t ref, std::vector<uint32_t>& worn, uint32_t& right, uint32_t&) override {
        if (ActorSet* a = actors()) {
            a->gear(ref, worn, right);
        }
    }
    int detection(uint32_t observer, uint32_t) override {
        const game::Agent* a = m_app.m_game->ai().agent(observer);
        return a ? a->detection : -1000;
    }
    // topic type 3 by subtype, for the speaker to say aloud. The picked line's own conditions decide.
    void comment(uint32_t actor, int subtype, uint32_t target) override {
        game::Dialogue* dialogue = m_app.m_papyrus ? m_app.m_papyrus->dialogue() : nullptr;
        if (game::Info* line = dialogue ? dialogue->pick_by_subtype(game::category_combat, uint16_t(subtype), actor, target) : nullptr; line && line->topic) {
            dialogue->say(actor, line->topic->form, target);
        }
    }
    void player_hit() override { m_app.player_struck(); }
    void died(uint32_t ref, uint32_t killer) override {
        game::Game& g = *m_app.m_game;
        g.ai().hold(ref, true);
        g.ai().drop(ref);
        if (ActorSet* a = actors()) {
            if (ref != game::k_player_ref) { // the player's body goes as the third-person view takes over (set_player)
                a->kill(ref); // its death animation, then the ragdoll
                if (a->death_refused(ref) && perform_action(ref, "ActionDeathWait")) {
                    a->death_taken(ref); // a dragon's: its own clip leads
                }
            }
        }
        if (ref == game::k_player_ref) {
            m_app.player_died();
        }
        g.script_event(ref, "OnDying", {{killer, "Actor"}});
        m_app.m_dying.push_back({ref, killer, 10.0f});
        if (game::QuestSystem* quests = m_app.m_papyrus ? m_app.m_papyrus->quests() : nullptr) {
            game::StoryEvent e; // V2, the rank before death, isn't given yet
            e.type = game::event_code("KILL");
            e.add("R1", ref);
            e.add("R2", killer);
            e.add("L1", g.conditions().location_of(ref));
            e.add("V1", killer == game::k_player_ref && !g.combat().fighter(ref).in_combat ? 1 : 0);
            quests->story().queue(e);
        }
    }
    void hit_event(const game::HitData& hit) override {
        if (m_app.m_papyrus) {
            m_app.m_papyrus->hit_event(hit.victim, hit.attacker, hit.weapon ? hit.weapon->form : 0, hit.source, hit.flags);
        }
    }
    bool follows_player(uint32_t ref) override {
        const game::Agent* a = m_app.m_game->ai().agent(ref);
        return a && (a->follows == game::k_player_ref || a->commander == game::k_player_ref);
    }
    // the eye's ray, slot 194. An actor on it is the target; anything else takes the impact.
    uint32_t swing_pick(uint32_t ref, float length, const game::Item* weapon, bool bash) override {
        game::ProjectileWorld* world = m_app.m_projectile_world.get();
        game::CombatBody self;
        if (!world || !body(ref, self)) {
            return 0;
        }
        game::Units3 eye{self.position[0], self.position[1], self.position[2] + self.height * 0.9f}, look{};
        if (ref == game::k_player_ref) {
            const glm::vec3 p = m_app.m_camera.position / units::k_skyrim_unit_m, f = m_app.m_camera.forward();
            eye = {p.x, p.y, p.z};
            look = {f.x, f.y, f.z};
        } else {
            const float heading = glm::radians(self.heading);
            look = {std::sin(heading), std::cos(heading), 0.0f};
        }
        const game::Units3 end{eye[0] + look[0] * length, eye[1] + look[1] * length, eye[2] + look[2] * length};
        for (const game::ProjectileContact& c : world->pick(eye, end, world->layer_row(k_layer_weapon))) {
            if (c.ref == ref) {
                continue;
            }
            if (c.actor) {
                return c.ref;
            }
            m_app.m_game->impacts().melee_object(ref, weapon, bash, c, look);
            return 0;
        }
        return 0;
    }

private:
    struct Bounds {
        int y1 = 0, y2 = 0, z1 = 0, z2 = 0;
        float height_scale = 1;
    };
    const Bounds& bounds(uint32_t npc) {
        const auto [it, added] = m_bounds.try_emplace(npc);
        if (added) {
            const forms::Database& db = m_app.m_game->db();
            if (const forms::FormEntry* e = db.find(npc)) {
                const forms::Form f(db, *e);
                const auto number = [&](std::string_view path) {
                    const forms::Value* v = f.get(path);
                    return v ? int(v->integer) : 0;
                };
                it->second.y1 = number("OBND.y1");
                it->second.y2 = number("OBND.y2");
                it->second.z1 = number("OBND.z1");
                it->second.z2 = number("OBND.z2");
                const forms::Value* h = f.get("NAM6");
                it->second.height_scale = h && h->real > 0.0 ? float(h->real) : 1.0f;
            }
        }
        return it->second;
    }

    App& m_app;
    std::unordered_map<uint32_t, Bounds> m_bounds;
};

// The caster's view of the world: where it looks and what's under the crosshair.
class EngineMagic final : public game::MagicWorld {
    // SummonCreatureEffect: the actor comes into the world by its commander, on what it was placed by.
    uint32_t summon(uint32_t caster, uint32_t npc, const game::Units3& at) override {
        game::Game& g = *m_app.m_game;
        const std::array<float, 3> where = {at[0], at[1], at[2]};
        const uint32_t id = g.place_form(npc, where);
        g.ai().add(id, npc, m_app.player_space(), {at[0], at[1], at[2]}, 0.0f, 1.0f);
        g.ai().command(id, caster);
        return id;
    }
    void fade_summon(uint32_t ref, float) override { m_app.m_game->ai().set_disabled(ref, true); }
    void reanimate(uint32_t corpse, uint32_t caster) override {
        game::Game& g = *m_app.m_game;
        if (ActorSet* actors = m_app.m_space ? m_app.m_space->actors() : nullptr) {
            actors->resurrect(corpse);
        }
        g.ai().hold(corpse, false);
        g.ai().command(corpse, caster);
    }
    void effect_applied(uint32_t target, uint32_t caster, uint32_t effect) override {
        if (m_app.m_papyrus) {
            script::VirtualMachine& vm = m_app.m_papyrus->vm();
            m_app.m_papyrus->ref_event(target, "OnMagicEffectApply", {vm.form_value(caster, "ObjectReference"), vm.form_value(effect, "MagicEffect")});
        }
    }
    void spell_cast_event(uint32_t actor, uint32_t item) override {
        if (m_app.m_papyrus) {
            m_app.m_papyrus->ref_event(actor, "OnSpellCast", {m_app.m_papyrus->vm().form_value(item, "Form")});
        }
    }
    // the Combat category's Shout subtypes.
    void say(uint32_t actor, int subtype) override {
        game::Dialogue* dialogue = m_app.m_papyrus ? m_app.m_papyrus->dialogue() : nullptr;
        if (game::Info* line = dialogue ? dialogue->pick_by_subtype(game::category_combat, uint16_t(subtype), actor, 0) : nullptr; line && line->topic) {
            dialogue->say(actor, line->topic->form, 0);
        }
    }
    void cast_noise(uint32_t actor, uint32_t level) override {
        if (actor == game::k_player_ref) {
            m_app.player_noise(int(level));
        }
    }
    void place_explosion(uint32_t form, const game::Units3& at, uint32_t caster) override {
        game::ExplosionSpawn spawn;
        spawn.form = form;
        spawn.source = caster;
        spawn.position = at;
        m_app.m_game->projectiles().explode(spawn);
    }
    void paralyse(uint32_t ref, bool on) override {
        game::Game& g = *m_app.m_game;
        g.ai().hold(ref, on);
        game::CombatBody body;
        if (on && ref != game::k_player_ref && g.combat().world().body(ref, body)) {
            g.combat().world().knockdown(ref, body.position, 0.0f); // down, as a ragdoll, until it wears off
        }
    }
    // Bound Weapon: the player's hand takes it, and what it held comes back after.
    void bound_weapon(uint32_t actor, uint32_t weapon, uint8_t hand) override {
        if (actor != game::k_player_ref) {
            return;
        }
        game::Game& g = *m_app.m_game;
        const bool left = hand == game::source_left;
        m_held_before[left] = g.equipped_weapon(left);
        m_spell_before[left] = g.casters().selected(game::k_player_ref, hand);
        g.casters().select(game::k_player_ref, hand, 0); // the weapon takes the hand from the spell
        g.player().inventory.add(weapon, 1);
        auto& stacks = g.player().inventory.stacks();
        for (size_t i = 0; i < stacks.size(); ++i) {
            if (stacks[i].form == weapon) {
                g.select(i, left ? 1 : 0);
                break;
            }
        }
    }
    void unbound_weapon(uint32_t actor, uint32_t weapon, uint8_t hand) override {
        if (actor != game::k_player_ref) {
            return;
        }
        game::Game& g = *m_app.m_game;
        const bool left = hand == game::source_left;
        game::Inventory& inventory = g.player().inventory;
        inventory.remove(weapon, 1);
        const uint32_t before = m_held_before[left];
        auto& stacks = inventory.stacks();
        for (size_t i = 0; before && i < stacks.size(); ++i) {
            if (stacks[i].form == before) {
                g.select(i, left ? 1 : 0);
                break;
            }
        }
        m_held_before[left] = 0;
        if (m_spell_before[left]) {
            g.casters().select(game::k_player_ref, hand, m_spell_before[left]);
            m_spell_before[left] = 0;
        }
    }
    std::array<uint32_t, 2> m_held_before{}, m_spell_before{}; // by hand, right first

    void light(uint32_t target, uint32_t form, bool on) override {
        if (m_app.m_vfx) {
            m_app.m_vfx->follow_light(target, on ? form : 0);
        }
    }

public:
    explicit EngineMagic(App& app) : m_app(app) {}

    bool look_angles(uint32_t ref, float& heading, float& pitch) override {
        if (ref == game::k_player_ref) {
            heading = glm::radians(m_app.m_camera.skyrim_heading_deg());
            pitch = glm::radians(m_app.m_camera.skyrim_pitch_deg());
            return true;
        }
        const game::Agent* a = m_app.m_game->ai().agent(ref);
        if (!a) {
            return false;
        }
        heading = a->heading;
        pitch = 0;
        return true;
    }
    bool combat_aim(uint32_t ref, uint32_t target, float& heading, float& pitch) override {
        game::CombatWorld& w = m_app.m_game->combat().world();
        game::CombatBody self, other;
        if (!w.body(ref, self) || !w.body(target, other)) {
            return false;
        }
        const float dx = other.position[0] - self.position[0], dy = other.position[1] - self.position[1];
        const float dz = other.position[2] + other.height * 0.5f - self.position[2] - self.height * 0.7f;
        heading = std::atan2(dx, dy);
        pitch = -std::atan2(dz, std::sqrt(dx * dx + dy * dy));
        return true;
    }
    bool view_ray(uint32_t, float units_long, game::MagicPick& out) override {
        const glm::vec3 forward = m_app.m_camera.forward();
        const auto hit = m_app.m_physics ? m_app.m_physics->raycast_shared(m_app.m_camera.position, forward,
                                                                           units_long * units::k_skyrim_unit_m)
                                         : std::nullopt;
        if (!hit) {
            return false;
        }
        const glm::vec3 p = (m_app.m_camera.position + forward * hit->distance) / units::k_skyrim_unit_m;
        out.point = {p.x, p.y, p.z};
        out.ref = m_app.m_crosshair_ref;
        return true;
    }
    uint32_t crosshair_actor(uint32_t) override {
        const uint32_t ref = m_app.m_crosshair_ref;
        return ref && m_app.m_game->ai().agent(ref) ? ref : 0;
    }
    void magic_hit(uint32_t target, uint32_t caster, uint32_t weapon) override {
        if (m_app.m_papyrus) {
            m_app.m_papyrus->hit_event(target, caster, weapon, 0, 0);
        }
    }
    void image_space(uint32_t imad) override {
        if (m_app.m_vfx) {
            m_app.m_vfx->modifier(imad, 1.0f, true);
        }
    }
    void pain(uint32_t ref) override {
        if (ref == game::k_player_ref) {
            m_app.m_game->impacts().player_hurt();
        }
    }
    void spawn_hazard(uint32_t hazard, uint32_t, const game::Units3& at, uint32_t caster) override {
        game::HazardSpawn s;
        s.form = hazard;
        s.source = caster;
        s.position = at;
        m_app.m_game->impacts().create_hazard(s);
    }
    void start_hit_effects(const game::ActiveEffect& e) override {
        const game::MagicEffect& m = *e.setting;
        if ((m.flags & game::effect_gory_visuals) && m_app.m_game->settings().number("bDisableAllGore", 0.0f) != 0.0f) {
            return;
        }
        RefEffects::Hit hit;
        hit.id = e.id;
        hit.target = e.target;
        hit.art = m.art_on_hit(e.dual);
        hit.shader = m.shader_on_hit(e.dual);
        hit.scale = m.hit_scale(e.dual);
        hit.persist = (m.flags & game::effect_fx_persist) != 0;
        m_app.ref_effects().start(hit);
        if (m.sounds[5]) {
            play_at(e.target, m.sounds[5]);
        }
    }
    void stop_hit_effects(uint32_t id) override { m_app.ref_effects().finish(id); }
    bool magic_node(uint32_t ref, uint8_t source, game::Units3& out) override {
        glm::vec3 at;
        if (!m_app.magic_node(ref, source, at)) {
            return false;
        }
        at /= units::k_skyrim_unit_m;
        out = {at.x, at.y, at.z};
        return true;
    }
    // Telekinesis: the object under the crosshair within its reach, held until the spell ends, then thrown.
    bool telekinesis_start(uint32_t caster) override {
        if (caster != game::k_player_ref) {
            return false;
        }
        const game::Settings& s = m_app.m_game->settings();
        const float reach = m_app.m_game->perk_value(23, s.number("fMagicTelekinesisBaseDistance", 2100.0f)) * units::k_skyrim_unit_m;
        const uint32_t ref = m_app.pick_loose(reach);
        if (!ref) {
            return false;
        }
        m_app.m_telekinesis = {};
        m_app.m_telekinesis.ref = ref;
        m_app.m_telekinesis.reach = reach;
        m_app.m_game->script_event(ref, "OnGrab");
        const auto at = m_app.m_physics->ref_position(ref);
        m_app.m_telekinesis.hold = at ? glm::length(*at - m_app.m_camera.position) : reach;
        return true;
    }
    void telekinesis_stop(uint32_t caster) override {
        App::Telekinesis& t = m_app.m_telekinesis;
        if (caster != game::k_player_ref || !t.ref || t.thrown) {
            return;
        }
        m_app.m_game->script_event(t.ref, "OnRelease");
        if (!t.armed) { // let go before it arrived: it just falls
            m_app.m_physics->steer_ref(t.ref, glm::vec3(0.0f));
            t = {};
            return;
        }
        const game::Settings& s = m_app.m_game->settings();
        t.thrown = true;
        t.dual = m_app.m_game->casters().dual_casting(caster);
        t.life = 3.0f; // long enough to land: what it hits in that time is hit
        const float speed = (s.number("fMagicTelekinesisThrow", 1000.0f) + t.charge) * (t.dual ? s.number("fMagicTelekinesisDualCastThrowMult", 2.0f) : 1.0f);
        m_app.m_physics->steer_ref(t.ref, m_app.m_camera.forward() * speed * units::k_skyrim_unit_m);
    }
    bool holding(uint32_t ref) override { return ref == game::k_player_ref && m_app.m_telekinesis.ref && !m_app.m_telekinesis.thrown; }
    // Clairvoyance: the nearest quest target, then the way there as hazards along the navmesh.
    int guide(uint32_t caster, uint32_t hazard) override {
        game::Game& g = *m_app.m_game;
        const auto from = g.ref_position(caster);
        std::optional<std::array<float, 3>> best;
        float nearest = std::numeric_limits<float>::max();
        if (from) {
            for (const App::QuestMark& m : m_app.m_quest_marks) { // the compass's: a target elsewhere is the door on the way
                const float d = std::hypot(m.walk.x - (*from)[0], m.walk.y - (*from)[1]);
                if (d < nearest) {
                    nearest = d;
                    best = std::array<float, 3>{m.walk.x, m.walk.y, m.walk.z};
                }
            }
        }
        if (!best) {
            return 1;
        }
        const uint32_t space = m_app.player_space();
        const auto start = g.navmeshes().locate(space, game::NavPoint{(*from)[0], (*from)[1], (*from)[2]});
        const auto end = g.navmeshes().locate(space, game::NavPoint{(*best)[0], (*best)[1], (*best)[2]});
        const std::vector<game::NavPoint> path = start && end ? g.navmeshes().path(*start, *end) : std::vector<game::NavPoint>{};
        if (path.empty()) {
            return 2;
        }
        const float spacing = g.settings().number("fMagicGuideSpacing", 46.0f);
        int left = int(g.settings().number("iMagicGuideWaypoints", 44.0f));
        float along = 0.0f;
        for (size_t i = 1; i < path.size() && left > 0; ++i) {
            const glm::vec3 a(path[i - 1][0], path[i - 1][1], path[i - 1][2]), b(path[i][0], path[i][1], path[i][2]);
            const float length = glm::length(b - a);
            for (; along < length && left > 0; along += spacing, --left) {
                const glm::vec3 p = glm::mix(a, b, along / std::max(length, 1.0f));
                spawn_hazard(hazard, caster, {p.x, p.y, p.z}, caster);
            }
            along -= length;
        }
        return 0;
    }
    void enhance_weapon(uint32_t actor, uint32_t ench, bool on) override { m_app.m_game->combat().fighter(actor).enhance = on ? ench : 0; }
    void hand_cast(uint32_t ref, uint8_t source, game::CastPhase phase) override {
        m_app.ref_effects().hand_cast(ref, source, phase);
        cast_audio(ref, source, phase);
    }
    // The costliest effect's sounds for where the cast stands: charge, ready and the concentration loop run until the next stage.
    void cast_audio(uint32_t ref, uint8_t source, game::CastPhase phase) {
        if (!m_app.m_audio) {
            return;
        }
        // A phase is heard once: the hand's update and the fire event both report a release.
        game::CastPhase& heard = m_cast_heard[uint64_t(ref) << 2 | source];
        if (heard == phase) {
            return;
        }
        heard = phase;
        audio::SoundId& loop = m_cast_loops[uint64_t(ref) << 2 | source];
        if (loop) {
            m_app.m_audio->stop(loop);
            loop = {};
        }
        game::Game& g = *m_app.m_game;
        const game::Spell* spell = g.casters().caster(ref, source).item();
        const game::SpellEffect* top = spell ? g.magic().costliest(*spell) : nullptr;
        const game::MagicEffect* m = top ? g.magic().effect(top->effect) : nullptr;
        const int type = phase == game::CastPhase::charging ? 1 : phase == game::CastPhase::ready ? 2 : phase == game::CastPhase::released ? 3
                         : phase == game::CastPhase::sustained ? 4 : -1;
        if (m && type >= 0 && m->sounds[size_t(type)]) {
            const audio::SoundId id = play_at(ref, m->sounds[size_t(type)]);
            if (type != 3) {
                loop = id;
            }
        }
    }
    audio::SoundId play_at(uint32_t ref, uint32_t sound) {
        game::CombatBody body;
        const auto descriptor = m_app.m_audio ? m_app.m_audio->find_descriptor(std::format("{:08X}", sound)) : std::nullopt;
        if (!descriptor || !m_app.m_game->combat().world().body(ref, body)) {
            return {};
        }
        return m_app.m_audio->play(*descriptor, glm::vec3(body.position[0], body.position[1], body.position[2]) * units::k_skyrim_unit_m, m_app.body_sound_model(ref));
    }
    std::unordered_map<uint64_t, audio::SoundId> m_cast_loops, m_hand_loops;
    std::unordered_map<uint64_t, game::CastPhase> m_cast_heard;
    // Slow Time: the world goes at its rate and the player at theirs.
    void slow_time(float player_rate, float world_rate, bool on) override {
        m_app.m_time_rate = on ? std::clamp(world_rate, 0.01f, 1.0f) : 1.0f;
        m_app.m_player_rate = on ? std::clamp(player_rate, 0.01f, 1.0f) : 1.0f;
    }
    bool time_slowed() override { return m_app.m_time_rate < 1.0f; }
    float time_factor() override { return m_app.m_time_rate; }
    // Detect Life: the hit shader on those in the area as it starts.
    void detect_life(uint32_t caster, uint32_t effect, bool on, float radius) override {
        std::vector<uint32_t>& shown = m_detected[effect];
        for (const uint32_t id : shown) {
            m_app.ref_effects().finish(id);
        }
        shown.clear();
        ActorSet* actors = m_app.m_space ? m_app.m_space->actors() : nullptr;
        const game::MagicEffect* m = m_app.m_game->magic().effect(effect);
        game::CombatBody self, other;
        game::CombatWorld& world = m_app.m_game->combat().world();
        if (!on || !actors || !m || !world.body(caster, self)) {
            return;
        }
        for (const ActorSet::Who& a : actors->who()) {
            if (a.ref == caster || m_app.m_game->combat().fighter(a.ref).dead() || !world.body(a.ref, other)) {
                continue;
            }
            const float dx = other.position[0] - self.position[0], dy = other.position[1] - self.position[1], dz = other.position[2] - self.position[2];
            if (std::sqrt(dx * dx + dy * dy + dz * dz) <= radius) {
                RefEffects::Hit hit;
                hit.id = m_app.m_game->next_effect_id();
                hit.target = a.ref;
                hit.shader = m->shader_on_hit(false);
                hit.persist = true;
                m_app.ref_effects().start(hit);
                shown.push_back(hit.id);
            }
        }
    }
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_detected;
    // Disarm: the weapon leaves the hand for the ground at the actor's feet.
    void disarm(uint32_t ref) override {
        game::Game& g = *m_app.m_game;
        const game::Item* w = g.combat().gear(ref).right.item;
        game::CombatBody body;
        if (!w || ref == game::k_player_ref || !g.combat().world().body(ref, body)) {
            return;
        }
        game::Stack stack;
        stack.form = w->form;
        stack.count = 1;
        const glm::vec3 spot = glm::vec3(body.position[0], body.position[1], body.position[2] + 40.0f) * units::k_skyrim_unit_m;
        const uint32_t id = g.place(stack, {spot.x, spot.y, spot.z});
        if (m_app.m_physics) {
            m_app.m_physics->add_object(id, w->model, spot, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        }
        g.inventory_of(ref).remove(w->form, 1);
        if (m_app.m_actor_data) {
            m_app.m_actor_data->npc_equip(ref, w->form, false); // a new look stays without it
        }
        g.combat().world().swap_weapon(ref, 0);
    }
    void hand_art(uint32_t ref, uint8_t source, const game::MagicEffect* m) override {
        m_app.ref_effects().hand(ref, source, m ? m->casting_art : 0, m ? m->casting_light : 0);
        // The draw sound runs with the art, under the cast's own sounds: one loop a hand, measured.
        if (m_app.m_audio) {
            audio::SoundId& loop = m_hand_loops[uint64_t(ref) << 2 | source];
            if (loop) {
                m_app.m_audio->stop(loop);
                loop = {};
            }
            if (m && m->sounds[0]) {
                loop = play_at(ref, m->sounds[0]);
            }
        }
    }
    void weapon_art(uint32_t ref, uint8_t source, const game::MagicEffect* m) override {
        m_app.ref_effects().weapon(ref, source, m ? m->enchant_art : 0, m ? m->enchant_shader : 0);
    }
    void default_object(uint32_t ref, int index) override {
        // The game's table, by DOBJ `use`. Only 204 is art; the others of these are sounds.
        const auto use = [index]() -> std::string_view {
            switch (index) {
            case 125: return "WBSN"; // Ward Break Sound
            case 126: return "WASN"; // Ward Absorb Sound
            case 133: return "SCSD"; // Soul Captured Sound
            case 183: return "TKGS"; // Telekinesis Grab Sound
            case 204: return "ABSE"; // Art Object - Absorb Effect
            default: return {};
            }
        }();
        const uint32_t form = default_form(use);
        if (index == 204) {
            m_app.ref_effects().play(ref, form);
            return;
        }
        game::CombatBody body;
        if (form && m_app.m_projectile_world && m_app.m_game->combat().world().body(ref, body)) {
            m_app.m_projectile_world->play_sound(form, &body.position);
        }
    }

private:
    uint32_t default_form(std::string_view use) {
        if (m_defaults.empty()) {
            const forms::Database& db = m_app.m_game->db();
            if (const forms::FormEntry* e = db.find(0x31)) { // The game; an override names only what it sets
                for (const forms::Value* v : forms::Form(db, *e).all("DNAM")) {
                    for (const forms::Value& item : v->items) {
                        if (const forms::Value* object = item.member("object"); object && object->form) {
                            m_defaults[uint32_t(item.member("use")->integer)] = object->form;
                        }
                    }
                }
            }
        }
        uint32_t key = 0;
        for (size_t i = use.size(); i-- > 0;) {
            key = key << 8 | uint8_t(use[i]);
        }
        const auto it = m_defaults.find(key);
        return it != m_defaults.end() ? it->second : 0;
    }

    App& m_app;
    std::unordered_map<uint32_t, uint32_t> m_defaults;
};

// Slot,: for the player, the body that shows.
bool App::magic_node(uint32_t ref, uint8_t source, glm::vec3& out) {
    const ActorSet* actors = m_space ? m_space->actors() : nullptr;
    glm::mat3 rotation;
    return actors && actors->node_world(ref, RefEffects::magic_node(source), rotation, out);
}

RefEffects& App::ref_effects() {
    if (!m_ref_effects) {
        m_ref_effects = std::make_unique<RefEffects>(m_game->db(), m_game->settings());
    }
    return *m_ref_effects;
}

void App::connect_combat() {
    m_ref_effects.reset();
    if (!m_game) {
        return;
    }
    m_combat_world = std::make_unique<EngineCombat>(*this);
    m_game->combat().set_world(m_combat_world.get());
    m_magic_world = std::make_unique<EngineMagic>(*this);
    m_game->set_magic_world(m_magic_world.get());
}

std::vector<ui::MagicRow> App::ui_magic_rows() {
    std::vector<ui::MagicRow> out;
    if (!m_game) {
        return out;
    }
    const game::Magic& magic = m_game->magic();
    const forms::Database& db = m_game->db();
    game::Casters& casters = m_game->casters();
    std::vector<uint32_t> known = m_game->character().added_spells();
    const auto record = m_game->actor_values(game::k_player_ref).record();
    for (const uint32_t owner : {record.npc, record.race}) {
        if (const forms::FormEntry* e = db.find(owner)) {
            for (const forms::Value* s : forms::Form(db, *e).all("SPLO")) {
                known.push_back(s->form);
            }
        }
    }
    std::ranges::sort(known);
    known.erase(std::ranges::unique(known).begin(), known.end());
    for (const uint32_t form : known) {
        ui::MagicRow row;
        row.form = form;
        const bool left = casters.selected(game::k_player_ref, game::source_left) == form;
        const bool right = casters.selected(game::k_player_ref, game::source_right) == form;
        const bool voice = casters.selected(game::k_player_ref, game::source_voice) == form;
        row.equip_state = left && right ? 4 : left ? 2 : right ? 3 : voice ? 1 : 0;
        if (const game::Spell* s = magic.spell(form)) {
            row.text = s->name;
            if (s->type == game::spell_power || s->type == game::spell_lesser_power) {
                row.filter_flag = 128;
            } else if (s->type == game::spell_plain) {
                // The school of the costliest effect.
                int skill = -1;
                float cost = -1;
                for (const game::SpellEffect& e : s->effects) {
                    const game::MagicEffect* m = magic.effect(e.effect);
                    if (const float c = magic.effect_cost(e); m && c > cost) {
                        cost = c;
                        skill = m->magic_skill;
                    }
                }
                row.filter_flag = skill == game::av_alteration    ? 2
                                  : skill == game::av_illusion    ? 4
                                  : skill == game::av_destruction ? 8
                                  : skill == game::av_conjuration ? 16
                                  : skill == game::av_restoration ? 32
                                                                  : 0;
            }
        } else if (const forms::FormEntry* e = db.find(form); e && e->type == forms::sig("SHOU")) {
            row.text = std::string(forms::Form(db, *e).name());
            row.filter_flag = 64;
        }
        if (!row.text.empty()) {
            out.push_back(std::move(row));
        }
    }
    std::ranges::sort(out, {}, &ui::MagicRow::text);
    return out;
}

uint32_t App::body_sound_model(uint32_t ref) const {
    return ref == game::k_player_ref && m_audio && m_player ? m_audio->player_output_model(m_player->view() == View::first_person) : osd::k_none;
}

bool App::ui_magic_equip(uint32_t form, int slot) {
    if (!m_game) {
        return false;
    }
    game::Casters& casters = m_game->casters();
    const game::Spell* s = m_game->magic().spell(form);
    if (!s || s->type == game::spell_power || s->type == game::spell_lesser_power) {
        casters.select(game::k_player_ref, game::source_voice, form);
    } else {
        m_game->equip_spell(form, slot == 1 ? 0 : slot == 2 ? 1 : -1);
    }
    return true;
}

// The pull gathers speed until the object is at hand, then the throw gathers strength; the object follows where the
// crosshair is at the distance it's held. Once thrown, the first actor it reaches takes the blow.
void App::update_telekinesis(float dt) {
    Telekinesis& t = m_telekinesis;
    if (!t.ref || !m_physics) {
        return;
    }
    const auto at = m_physics->ref_position(t.ref);
    if (!at) {
        t = {};
        return;
    }
    const game::Settings& s = m_game->settings();
    if (!t.thrown) {
        const float nearest = s.number("fMagicTelekinesisDistanceMin", 64.0f) * units::k_skyrim_unit_m;
        const float fastest = s.number("fMagicTelekinesisMoveMax", 1000.0f);
        if (!t.at_hand) {
            t.speed = std::min(std::max(t.speed, s.number("fMagicTelekinesisMoveBase", 50.0f)) +
                                   s.number("fMagicTelekinesisMoveAccelerate", 400.0f) * dt, fastest);
        } else {
            t.speed = 0.0f;
            if (t.charge < s.number("fMagicTelekinesisThrowMax", 30.0f)) {
                t.charge += s.number("fMagicTelekinesisThrowAccelerate", 10.0f) * dt;
                t.armed = t.armed || t.charge > 0.0f;
            }
        }
        t.hold -= t.speed * units::k_skyrim_unit_m * dt;
        if (t.hold < nearest) {
            t.hold = nearest;
            t.at_hand = true;
        }
        t.hold = std::min(t.hold, t.reach);
        const glm::vec3 to = m_camera.position + m_camera.forward() * t.hold - *at;
        const float distance = glm::length(to);
        const float chase = std::min(fastest * units::k_skyrim_unit_m, distance * 8.0f); // eased, or each step overshoots
        m_physics->steer_ref(t.ref, distance > 0.001f ? to / distance * chase : glm::vec3(0.0f));
        return;
    }
    if ((t.life -= dt) <= 0.0f) {
        t = {};
        return;
    }
    ActorSet* actors = m_space ? m_space->actors() : nullptr;
    if (!actors) {
        return;
    }
    game::CombatBody body;
    game::CombatWorld& world = m_game->combat().world();
    const glm::vec3 units = *at / units::k_skyrim_unit_m;
    for (const ActorSet::Who& a : actors->who()) {
        if (a.ref == game::k_player_ref || !world.body(a.ref, body)) {
            continue;
        }
        const float across = std::hypot(body.position[0] - units.x, body.position[1] - units.y);
        if (across < 40.0f && units.z > body.position[2] && units.z < body.position[2] + 130.0f) {
            const float mult = t.dual ? s.number("fMagicTelekinesisDualCastDamageMult", 2.0f) : s.number("fMagicTelekinesisDamageMult", 1.0f);
            const float damage = m_game->perk_value(25, s.number("fMagicTelekinesisDamageBase", 5.0f)) * m_game->perk_value(24, mult);
            const game::Units3 where = {units.x, units.y, units.z};
            m_game->combat().projectile_hit(game::k_player_ref, a.ref, 0, nullptr, damage, false, where, {0.0f, 0.0f, 0.0f});
            t = {};
            return;
        }
    }
}

void App::update_combat(float dt) {
    if (!m_game || (m_ui && m_ui->pausing_menus() > 0)) {
        return;
    }
    game::Combat& combat = m_game->combat();
    update_telekinesis(dt);
    // OnDeath, as dying ends: the ragdoll has taken over, the 3D has gone, or it never had any. 10 s is *inferred*.
    for (size_t i = m_dying.size(); i-- > 0;) {
        Dying& d = m_dying[i];
        ActorSet* actors = m_space ? m_space->actors() : nullptr;
        if ((d.left -= dt) <= 0.0f || !actors || !actors->dying(d.ref)) {
            const Dying done = d;
            m_dying.erase(m_dying.begin() + std::ptrdiff_t(i));
            m_game->script_event(done.ref, "OnDeath", {{done.killer, "Actor"}});
        }
    }
    if (ActorSet* actors = m_space ? m_space->actors() : nullptr) {
        for (const ActorSet::Who& a : actors->who()) {
            if (!combat.fighter(a.ref).weapon_drawn) {
                continue;
            }
            if (const float speed = combat.weapon_speed(a.ref, false); m_sent_speed[a.ref] != speed) {
                m_sent_speed[a.ref] = speed;
                actors->set_variable(a.ref, "weaponSpeedMult", speed);
            }
        }
        const float speed = combat.weapon_speed(game::k_player_ref, false);
        if (m_combat_world && m_sent_speed[game::k_player_ref] != speed) {
            m_sent_speed[game::k_player_ref] = speed;
            m_combat_world->graph_float(game::k_player_ref, "weaponSpeedMult", speed);
        }
    }
    if (m_player) {
        game::Fighter& f = combat.fighter(game::k_player_ref);
        const auto& s = m_player->status();
        f.sneaking = s.sneaking;
        f.swimming = s.swimming;
        f.running = s.sprinting;
    }
    combat.difficulty = int(m_user.number("iDifficulty:GamePlay"));
    combat.update(dt);
    if (m_ai_ready) {
        m_game->combat_ai().update(dt);
    }
    update_attack_input(dt);
}

// buttons to actions.
void App::update_attack_input(float dt) {
    if (player_down()) {
        return;
    }
    if (!m_walk || !m_player || !m_window || !m_focus.game() || !m_combat_world) {
        m_attack_held = {};
        return;
    }
    const auto down = [&](const char* event, int fallback) {
        const osr::KeyBinding* b = m_rules ? m_rules->key(event) : nullptr;
        const uint32_t mouse = mouse_state();
        const int button = b && b->mouse != 0xFF ? int(b->mouse) : fallback;
        return (mouse & SDL_BUTTON_MASK(button == 0 ? SDL_BUTTON_LEFT : SDL_BUTTON_RIGHT)) != 0;
    };
    game::CombatWorld& w = *m_combat_world;
    game::Fighter& f = m_game->combat().fighter(game::k_player_ref);
    const game::Settings& s = m_game->settings();
    const float initial = s.number("fInitialPowerAttackDelay", 0.3f), later = s.number("fSubsequentPowerAttackDelay", 2.0f);
    std::array<bool, 2> now = {down("Left Attack/Block", 1) || pad_held("Left Attack/Block"),
                               down("Right Attack/Block", 0) || pad_held("Right Attack/Block")};
    for (size_t hand = 0; hand < 2; ++hand) {
        if (m_script_attack[hand] > 0.0f) {
            m_script_attack[hand] -= dt;
            now[hand] = true;
        }
    }
    game::Casters& casters = m_game->casters();
    bool hands_changed = false;
    // The graph's hands follow what they hold, spells too: the action tree and the magic stance read them.
    for (const int hand : {0, 1}) {
        const uint8_t source = hand == 0 ? game::source_left : game::source_right;
        const game::Gear gear = m_game->combat().gear(game::k_player_ref);
        const game::Item* item = hand == 1 ? gear.right.item : gear.left.item;
        const int type = casters.selected(game::k_player_ref, source) ? game::k_hand_spell : item ? item->animation_type : 0;
        // Asked of the graph itself: a change of gear brings a new one that knows nothing of its hands.
        const char* name = hand == 0 ? "iLeftHandType" : "iRightHandType";
        int held = -1;
        if (w.graph_value(game::k_player_ref, name, held) && held != type) {
            w.graph_int(game::k_player_ref, name, type);
            hands_changed = true;
            m_game->combat().replaced_item[size_t(hand)] = held;
        }
        // Actor's weapon state: what the hands hold counts only with them out.
        const char* out = hand == 0 ? "iLeftHandEquipped" : "iRightHandEquipped";
        if (const int value = f.weapon_drawn ? type : 0; w.graph_value(game::k_player_ref, out, held) && held != value) {
            w.graph_int(game::k_player_ref, out, value);
        }
    }
    int magic = 0;
    const int magic_out = f.weapon_drawn && (casters.selected(game::k_player_ref, game::source_left) ||
                                             casters.selected(game::k_player_ref, game::source_right));
    if (w.graph_value(game::k_player_ref, "iMagicEquipped", magic) && magic != magic_out) {
        w.graph_int(game::k_player_ref, "iMagicEquipped", magic_out);
    }
    // A hand change on the player keeps the weapon out. A new body has to draw; the graph that stayed
    // runs the force equip tree, whose branches read what was replaced (MagicForceEquipRight, WeapOutRightReplaceForceEquip).
    if (m_redraw_weapon) {
        m_redraw_weapon = !w.perform_action(game::k_player_ref, "ActionDraw");
    } else if (hands_changed && f.weapon_drawn) {
        w.perform_action(game::k_player_ref, "ActionForceEquip");
    }
    m_game->combat().replaced_item = {-1, -1};
    const bool* keys = key_state();
    const osr::KeyBinding* shout = m_rules ? m_rules->key("Shout") : nullptr;
    const SDL_Scancode voice_key = shout ? scancode_from_dik(shout->keys[0]) : SDL_SCANCODE_Z;
    const bool scripted_voice = m_script_attack[2] > 0.0f;
    m_script_attack[2] = std::max(m_script_attack[2] - dt, 0.0f);
    casters.set_keys(now[0], now[1], (voice_key != SDL_SCANCODE_UNKNOWN && keys[voice_key]) || pad_held("Shout") || scripted_voice);
    for (size_t hand = 0; hand < 2; ++hand) {
        AttackHold& h = m_attack_held[hand];
        // A spell in that hand goes to the hand function, and nothing below runs.
        if (f.weapon_drawn && casters.selected(game::k_player_ref, hand == 0 ? game::source_left : game::source_right)) {
            h = {};
            continue;
        }
        const bool pressed = now[hand] && h.seconds < 0.0f, held = now[hand] && h.seconds >= 0.0f, released = !now[hand] && h.seconds >= 0.0f;
        if (pressed) {
            h.seconds = 0;
            h.next_power = initial;
            if (!f.weapon_drawn) {
                f.weapon_drawn = true; // a press draws and does nothing else
                w.perform_action(game::k_player_ref, "ActionDraw");
                h.armed = false;
            } else {
                w.perform_action(game::k_player_ref, hand == 0 ? "ActionLeftAttack" : "ActionRightAttack");
                h.armed = hand == 1 || !w.graph_bool(game::k_player_ref, "IsBlocking");
            }
        } else if (held) {
            h.seconds += dt;
            if (h.armed && h.seconds >= h.next_power) {
                h.next_power += later;
                w.perform_action(game::k_player_ref, hand == 0 ? "ActionLeftPowerAttack" : "ActionRightPowerAttack");
            }
        } else if (released) {
            h.seconds = -1;
            if (f.weapon_drawn) {
                w.perform_action(game::k_player_ref, hand == 0 ? "ActionLeftRelease" : "ActionRightRelease");
            }
        }
    }
}

} // namespace spud::engine
