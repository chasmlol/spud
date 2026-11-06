// The natives game scripts call that had no other home: small questions about state the engine already keeps. `game stubs`
// lists what's left. Effects with no system behind them yet answer as the wiki's documentation of the native says they
// return, and are named.
#include "engine/game_natives.hpp"

#include "core/log.hpp"
#include "formats/forms_typed.hpp"
#include "core/game_random.hpp"
#include "game/game.hpp"
#include "game/quests.hpp"
#include "game/scenes.hpp"
#include "script/natives.hpp"
#include "script/vm.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace spud::engine {

namespace {

using game::k_player_ref;
using script::NativeCall;
using script::Value;

constexpr uint16_t k_get_light_level = 711, k_has_family_relationship = 259, k_has_parent_relationship = 261, k_get_faction_reaction = 410;

uint32_t form_of(const Value& v) { return v.object ? script::handle_form(v.object->handle) : 0; }
uint32_t self_form(const NativeCall& c) { return c.self ? script::handle_form(c.self->handle) : 0; }

} // namespace

void bind_misc_natives(script::VirtualMachine& vm, game::Game& g, const ReferenceHost& host) {
    const auto bind = [&vm](const char* script, const char* name, script::NativeFunction fn) {
        vm.bind_native(script, name, std::move(fn), script::native_flags(script, name));
    };
    game::Combat& combat = g.combat();
    const auto condition = [&g](uint16_t function, uint32_t subject, uint32_t p1) {
        return g.conditions().value(function, subject, 0, p1, 0);
    };

    // ---- small facts ----
    bind("Sound", "SetInstanceVolume", [host](NativeCall& c) {
        if (c.args[0].integer == 0) {
            c.vm.log_error(c.stack, "Attempting to set the volume of an invalid sound instance");
            return;
        }
        host.sound_volume(uint32_t(c.args[0].integer), float(c.args[1].real));
    });
    bind("ShaderParticleGeometry", "Apply", [host](NativeCall& c) { host.particles(self_form(c), float(c.args[0].real), true); });
    bind("ShaderParticleGeometry", "Remove", [host](NativeCall& c) { host.particles(self_form(c), float(c.args[0].real), false); });
    bind("Actor", "IsChild", [&g](NativeCall& c) { c.result = Value::of(g.is_child(g.base_of(self_form(c)))); });
    bind("Cell", "IsAttached", [host](NativeCall& c) { c.result = Value::of(host.cell_attached(self_form(c))); });
    bind("ObjectReference", "GetMass", [host](NativeCall& c) { c.result = Value::of(host.mass(self_form(c))); });
    bind("ObjectReference", "GetKey", [&g](NativeCall& c) { c.result = c.vm.form_value(g.inspect(self_form(c)).key, "Key"); });
    bind("ObjectReference", "GetCurrentScene", [&g](NativeCall& c) {
        game::QuestSystem* quests = g.quests();
        c.result = c.vm.form_value(quests ? quests->scenes().scene_of(self_form(c)) : 0, "Scene");
    });
    bind("Location", "HasRefType", [&g](NativeCall& c) {
        game::QuestSystem* quests = g.quests();
        const uint32_t type = form_of(c.args[0]);
        if (!type) {
            c.vm.log_error(c.stack, "Cannot check if the reference has a None location ref type");
            c.result = Value::of(false);
            return;
        }
        c.result = Value::of(quests && !quests->world().location_refs(self_form(c), type).empty());
    });
    bind("Weather", "FindWeather", [host](NativeCall& c) { c.result = c.vm.form_value(host.find_weather(int(c.args[0].integer)), "Weather"); });
    bind("Weather", "GetSkyMode", [host](NativeCall& c) { c.result = Value::of(int32_t(host.sky_mode())); });
    bind("Game", "ClearPrison", [&g](NativeCall&) { g.crime().clear_prison(); });
    bind("Game", "QuitToMainMenu", [host](NativeCall&) { host.quit_to_menu(); });
    bind("Faction", "SetReaction", [&g](NativeCall& c) { g.crime().set_faction_reaction(self_form(c), form_of(c.args[0]), int(c.args[1].integer)); });

    // ---- what an actor is doing ----
    bind("Actor", "IsRunning", [&combat, &g](NativeCall& c) {
        const uint32_t self = self_form(c);
        const game::Agent* a = self == k_player_ref ? nullptr : g.ai().agent(self);
        c.result = Value::of(self == k_player_ref ? combat.fighter(self).running : a && a->next < a->path.size() && a->speed >= g.ai().run_speed());
    });
    bind("Actor", "IsSneaking", [&combat](NativeCall& c) { c.result = Value::of(combat.fighter(self_form(c)).sneaking); });
    bind("Actor", "StartSneaking", [&combat](NativeCall& c) { combat.fighter(self_form(c)).sneaking = true; });
    bind("Actor", "GetEquippedShield", [&combat](NativeCall& c) {
        const game::Item* shield = combat.gear(self_form(c)).shield;
        c.result = c.vm.form_value(shield ? shield->form : 0, "Armor");
    });
    bind("Actor", "WornHasKeyword", [&combat](NativeCall& c) {
        const uint32_t k = form_of(c.args[0]);
        const game::Gear gear = combat.gear(self_form(c));
        c.result = Value::of(std::ranges::any_of(gear.armor, [k](const game::Item* item) { return item->has_keyword(k); }));
    });
    bind("Actor", "GetGoldAmount", [&g](NativeCall& c) {
        const uint32_t self = self_form(c);
        c.result = Value::of(int32_t(g.inventory_of(self).count(0xF)));
    });
    bind("Actor", "IsHostileToActor", [&g, &combat](NativeCall& c) {
        const uint32_t self = self_form(c), other = form_of(c.args[0]);
        c.result = Value::of(combat.in_combat_with(self, other) || g.combat_ai().should_attack(self, g.base_of(self), other, g.base_of(other), 0.0f));
    });
    bind("Actor", "HasFamilyRelationship", [condition](NativeCall& c) {
        c.result = Value::of(condition(k_has_family_relationship, self_form(c), form_of(c.args[0])) != 0.0);
    });
    bind("Actor", "HasParentRelationship", [condition](NativeCall& c) {
        c.result = Value::of(condition(k_has_parent_relationship, self_form(c), form_of(c.args[0])) != 0.0);
    });
    bind("Actor", "GetFactionReaction", [condition](NativeCall& c) {
        c.result = Value::of(int32_t(condition(k_get_faction_reaction, self_form(c), form_of(c.args[0]))));
    });
    bind("Actor", "GetLightLevel", [condition](NativeCall& c) { c.result = Value::of(float(condition(k_get_light_level, self_form(c), 0))); });
    bind("Actor", "GetBribeAmount", [&g](NativeCall& c) { c.result = Value::of(g.crime().bribe_amount(g.base_of(self_form(c)))); });
    bind("Actor", "SetBribed", [&g](NativeCall& c) { g.set_ref_flag(self_form(c), game::ref_bribed, c.args.empty() || c.args[0].boolean); });
    bind("Actor", "SetIntimidated", [&g](NativeCall& c) { g.set_ref_flag(self_form(c), game::ref_intimidated, c.args.empty() || c.args[0].boolean); });
    bind("Actor", "SetDontMove", [&g](NativeCall& c) { g.ai().hold(self_form(c), c.args.empty() || c.args[0].boolean); });
    bind("Actor", "CanFlyHere", [host](NativeCall& c) { c.result = Value::of(!host.in_interior || !host.in_interior()); });
    bind("Actor", "SetAllowFlyingEx", [&g](NativeCall& c) {
        if (game::Agent* a = g.ai().mutable_agent(self_form(c))) {
            a->allow_flying = c.args.empty() || c.args[0].boolean;
            a->flight.line.clear();
        }
    });
    bind("Actor", "ClearArrested", [&g](NativeCall& c) {
        if (game::Agent* a = g.ai().mutable_agent(self_form(c))) {
            a->arresting = false;
        }
    });
    // PathToReference: walks there, and the call returns when it has (or can't).
    bind("Actor", "PathToReference", [&g](NativeCall& c) {
        const uint32_t self = self_form(c), target = form_of(c.args[0]);
        g.ai().close_on(self, target, 128.0f);
        c.result = Value::of(true);
        c.vm.return_from_latent(c.stack, Value::of(true));
    });
    bind("Actor", "UnequipItemSlot", [&g](NativeCall& c) {
        if (self_form(c) == k_player_ref) {
            const uint32_t bit = 1u << uint32_t(std::max(c.args[0].integer - 30, 0));
            g.unequip_where([bit](const game::Item& item, const game::Stack&) { return (item.biped_slots & bit) != 0; });
        }
    });
    for (const char* name : {"SendVampirismStateChanged", "SendLycanthropyStateChanged"}) {
        const std::string event = name[4] == 'V' ? "OnVampirismStateChanged" : "OnLycanthropyStateChanged";
        bind("Actor", name, [host, event](NativeCall& c) {
            if (host.bool_event) {
                host.bool_event(self_form(c), event, c.args.empty() || c.args[0].boolean);
            }
        });
    }

    // ---- references ----
    bind("ObjectReference", "GetVoiceType", [&g](NativeCall& c) {
        const forms::FormEntry* e = g.db().find(g.base_of(self_form(c)));
        const forms::Value* v = e ? forms::Form(g.db(), *e).get("VTCK") : nullptr;
        c.result = c.vm.form_value(v ? v->form : 0, "VoiceType");
    });
    bind("ObjectReference", "IsDeleted", [&g](NativeCall& c) { c.result = Value::of(g.base_of(self_form(c)) == 0); });
    bind("ObjectReference", "InterruptCast", [&g](NativeCall& c) {
        for (const uint8_t source : {game::source_left, game::source_right, game::source_voice}) {
            g.casters().caster(self_form(c), source).interrupt(false);
        }
    });
    bind("ObjectReference", "GetAnimationVariableBool", [host](NativeCall& c) { c.result = Value::of(host.graph_float && host.graph_float(self_form(c), std::string(c.vm.text(c.args[0].string))) != 0.0f); });
    bind("ObjectReference", "GetAnimationVariableFloat", [host](NativeCall& c) { c.result = Value::of(host.graph_float ? host.graph_float(self_form(c), std::string(c.vm.text(c.args[0].string))) : 0.0f); });
    bind("ObjectReference", "Reset", [&g, host](NativeCall& c) {
        const uint32_t self = self_form(c);
        g.reset_ref(self);
        if (host.moved) {
            host.moved(self);
        }
        c.vm.return_from_latent(c.stack, {});
    });

    // ---- the world ----
    const auto nearest = [host](NativeCall& c, std::vector<uint32_t> bases, bool random, const char* type, bool actors) {
        const std::array<float, 3> at{c.args[actors ? 0 : 1].real, c.args[actors ? 1 : 2].real, c.args[actors ? 2 : 3].real};
        const float radius = c.args[actors ? 3 : 4].real;
        const std::vector<uint32_t> found = host.refs_near ? host.refs_near(at, radius, std::move(bases), actors) : std::vector<uint32_t>{};
        const uint32_t pick = found.empty() ? 0 : random ? found[size_t(GameRandom::shared().next_below(uint32_t(found.size())))] : found.front();
        c.result = c.vm.form_value(pick, type);
    };
    const auto listed = [&g](uint32_t list) {
        std::vector<uint32_t> out;
        if (const forms::FormEntry* e = g.db().find(list); e && e->type == forms::sig("FLST")) {
            out = forms::form_list(forms::Form(g.db(), *e));
        }
        return out;
    };
    bind("Game", "FindClosestReferenceOfType", [nearest](NativeCall& c) { nearest(c, {form_of(c.args[0])}, false, "ObjectReference", false); });
    bind("Game", "FindRandomReferenceOfType", [nearest](NativeCall& c) { nearest(c, {form_of(c.args[0])}, true, "ObjectReference", false); });
    bind("Game", "FindClosestReferenceOfAnyTypeInList", [nearest, listed](NativeCall& c) { nearest(c, listed(form_of(c.args[0])), false, "ObjectReference", false); });
    bind("Game", "FindRandomReferenceOfAnyTypeInList", [nearest, listed](NativeCall& c) { nearest(c, listed(form_of(c.args[0])), true, "ObjectReference", false); });
    bind("Game", "FindClosestActor", [nearest](NativeCall& c) { nearest(c, {}, false, "Actor", true); });
    bind("Game", "FindRandomActor", [nearest](NativeCall& c) { nearest(c, {}, true, "Actor", true); });
    bind("Game", "UsingGamepad", [host](NativeCall& c) { c.result = Value::of(host.using_gamepad && host.using_gamepad()); });
    bind("Game", "SetPlayerAIDriven", [host](NativeCall& c) {
        if (host.player_ai_driven) {
            host.player_ai_driven(c.args.empty() || c.args[0].boolean);
        }
    });
    bind("Game", "SetHudCartMode", [host](NativeCall& c) { host.hud_mode("CartMode", c.args[0].boolean); });
    bind("Game", "SetSittingRotation", [host](NativeCall& c) { host.sitting_rotation(float(c.args[0].real)); });
    bind("Actor", "SetVehicle", [host](NativeCall& c) { host.set_vehicle(self_form(c), form_of(c.args[0])); });
    bind("Game", "ServeTime", [&g](NativeCall&) { g.crime().serve_sentence(); });
    bind("Game", "GetPlayerGrabbedRef", [](NativeCall& c) { c.result = c.vm.form_value(0, "ObjectReference"); });
    // Screen and start-up effects with nothing behind them here: they change no game state a script reads back.
    for (const char* name : {"ClearTempEffects", "PlayBink", "PrecacheCharGen", "PrecacheCharGenClear", "TriggerScreenBlood", "SetSunGazeImageSpaceModifier",
                             "ShowTitleSequenceMenu", "HideTitleSequenceMenu", "StartTitleSequence"}) {
        bind("Game", name, [](NativeCall&) {});
    }

    // ---- weather, music, sound ----
    bind("Weather", "SetActive", [host](NativeCall& c) {
        if (host.set_weather) {
            host.set_weather(self_form(c), c.args.size() > 1 && c.args[1].boolean);
        }
    });
    bind("MusicType", "Add", [host](NativeCall& c) {
        if (host.music) {
            host.music(self_form(c), true);
        }
    });
    bind("MusicType", "Remove", [host](NativeCall& c) {
        if (host.music) {
            host.music(self_form(c), false);
        }
    });

    // ---- records ----
    bind("Faction", "GetCrimeGoldViolent", [&g](NativeCall& c) { c.result = Value::of(g.crime().bounty(self_form(c)).violent); });
    bind("Faction", "GetCrimeGoldNonViolent", [&g](NativeCall& c) { c.result = Value::of(g.crime().bounty(self_form(c)).non_violent); });
    bind("ActorBase", "IsUnique", [&g](NativeCall& c) { c.result = Value::of(g.base_flag(self_form(c), 0x20)); });
    bind("Armor", "GetWarmthRating", [&g](NativeCall& c) {
        const game::Item* item = g.items().find(self_form(c));
        c.result = Value::of(item ? g.item_warmth(*item) : 0.0f);
    });
    bind("Light", "GetWarmthRating", [&g](NativeCall& c) { c.result = Value::of(g.settings().number("fSurvTorchBonus", 50.0f)); });
    bind("Location", "IsLoaded", [host](NativeCall& c) { c.result = Value::of(host.location_loaded && host.location_loaded(self_form(c))); });
}

} // namespace spud::engine
