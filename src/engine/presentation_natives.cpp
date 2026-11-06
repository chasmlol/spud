// Papyrus natives the world answers rather than a record: animation graphs, image-space modifiers, sounds, weather, the
// camera and the menus scripts open.

#include "engine/game_natives.hpp"

#include "core/log.hpp"
#include "formats/forms_typed.hpp"
#include "game/game.hpp"
#include "script/natives.hpp"
#include "script/vm.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <memory>
#include <string>

namespace spud::engine {

namespace {

using game::k_player_ref;
using script::NativeCall;
using script::Tag;
using script::Value;

uint32_t form_of(const Value& v) {
    return v.tag == Tag::object && v.object ? script::handle_form(v.object->handle) : 0;
}

uint32_t self_form(const NativeCall& c) { return c.self ? script::handle_form(c.self->handle) : 0; }

std::string_view text_of(NativeCall& c, const Value& v) { return v.tag == Tag::string ? c.vm.text(v.string) : std::string_view(); }

bool is_actor(const game::Game& g, uint32_t ref) {
    const forms::FormEntry* e = g.db().find(ref);
    return ref == k_player_ref || (e && e->type == forms::sig("ACHR"));
}

std::string editor_id(const game::Game& g, uint32_t form) {
    const forms::FormEntry* e = g.db().find(form);
    return e ? std::string(g.db().editor_id(*e)) : std::string();
}

} // namespace

void bind_presentation_natives(script::VirtualMachine& vm, game::Game& g, const ReferenceHost& host) {
    const auto bind = [&vm](const char* cls, const char* name, script::NativeFunction fn) {
        vm.bind_native(cls, name, std::move(fn), script::native_flags(cls, name));
    };

    // ---- idles and graphs ----
    bind("Actor", "SetLookAt", [host](NativeCall& c) { host.look_at(self_form(c), form_of(c.args[0])); });
    bind("Actor", "ClearLookAt", [host](NativeCall& c) { host.look_at(self_form(c), 0); });
    for (const char* name : {"PlayIdle", "PlayIdleWithTarget"}) {
        bind("Actor", name, [&g, host](NativeCall& c) {
            const uint32_t self = self_form(c), idle = form_of(c.args[0]);
            if (!idle) {
                c.vm.log_error(c.stack, "Cannot play a None idle on an actor");
                c.result = Value::of(false);
                return;
            }
            // Refused while getting up or down, and while restrained.
            const int32_t sit = g.sit_state(self), sleep = g.sleep_state(self);
            const bool settled = (sit == 0 || sit == 3) && (sleep == 0 || sleep == 3);
            c.result = Value::of(settled && !g.ref_flag(self, game::ref_restrained) && host.play_idle(self, editor_id(g, idle)));
        });
    }
    bind("ObjectReference", "PlayAnimation", [&g, host](NativeCall& c) {
        const uint32_t self = self_form(c);
        if (is_actor(g, self)) {
            c.vm.log_error(c.stack, std::format("{:08X} is an Actor, and PlayAnimation should not be used.", self));
            c.result = Value::of(false);
            return;
        }
        c.result = Value::of(host.animation_event(self, text_of(c, c.args[0])));
    });
    bind("ObjectReference", "PlayAnimationAndWait", [&g, host](NativeCall& c) {
        const uint32_t self = self_form(c);
        if (is_actor(g, self)) {
            c.vm.log_error(c.stack, std::format("{:08X} is available only for non-actors.  Returning immediately.", self));
            c.vm.return_from_latent(c.stack, Value::of(false));
            return;
        }
        // No graph took it: the game returns at once too. Otherwise the stack sleeps until the graph sends the event.
        if (!host.animation_event(self, text_of(c, c.args[0]))) {
            c.vm.return_from_latent(c.stack, Value::of(false));
            return;
        }
        host.wait_animation(self, text_of(c, c.args[1]), c.stack);
    });
    bind("ObjectReference", "WaitForAnimationEvent", [host](NativeCall& c) {
        host.wait_animation(self_form(c), text_of(c, c.args[0]), c.stack);
    });
    bind("ObjectReference", "PlayGamebryoAnimation", [host](NativeCall& c) {
        const std::string name(text_of(c, c.args[0]));
        const int played = host.play_gamebryo(self_form(c), name, c.args[1].boolean, c.args[2].real);
        if (played == 1) {
            c.vm.log_error(c.stack, std::format("{:08X} has no 3d and cannot have an animation played on it", self_form(c)));
        } else if (played == 2) {
            c.vm.log_error(c.stack, std::format("{:08X} has no animation controller manager and cannot have an animation played on it", self_form(c)));
        } else if (played == 3) {
            c.vm.log_error(c.stack, std::format("{:08X} does not have an animation named \"{}\"", self_form(c), name));
        }
        c.result = Value::of(played == 0);
    });
    bind("Debug", "SendAnimationEvent", [host](NativeCall& c) {
        if (const uint32_t ref = form_of(c.args[0])) {
            host.animation_event(ref, text_of(c, c.args[1]));
        }
    });
    bind("Actor", "PlaySubGraphAnimation", [host](NativeCall& c) {
        if (!host.animation_event(self_form(c), text_of(c, c.args[0]))) {
            c.vm.log_error(c.stack, std::format("Failed to send event {} for unspecified reasons", text_of(c, c.args[0])));
        }
    });
    bind("Actor", "SetSubGraphFloatVariable", [host](NativeCall& c) {
        host.graph_variable(self_form(c), text_of(c, c.args[0]), c.args[1].real);
    });
    const auto set_variable = [host](NativeCall& c, float value) {
        if (!host.graph_variable(self_form(c), text_of(c, c.args[0]), value)) {
            c.vm.log_error(c.stack, std::format("{:08X} cannot find variable named {}", self_form(c), text_of(c, c.args[0])));
        }
    };
    bind("ObjectReference", "SetAnimationVariableBool", [set_variable](NativeCall& c) { set_variable(c, c.args[1].boolean ? 1.0f : 0.0f); });
    bind("ObjectReference", "SetAnimationVariableInt", [set_variable](NativeCall& c) { set_variable(c, float(c.args[1].integer)); });
    bind("ObjectReference", "SetAnimationVariableFloat", [set_variable](NativeCall& c) { set_variable(c, c.args[1].real); });
    for (const char* cls : {"Form", "Alias", "ActiveMagicEffect"}) {
        bind(cls, "RegisterForAnimationEvent", [host](NativeCall& c) {
            const uint32_t sender = form_of(c.args[0]);
            if (!sender) {
                c.vm.log_error(c.stack, "Cannot register for an animation event on an None object");
                c.result = Value::of(false);
                return;
            }
            host.register_animation(c.self->handle, sender, text_of(c, c.args[1]), true);
            c.result = Value::of(true);
        });
        bind(cls, "UnregisterForAnimationEvent", [host](NativeCall& c) {
            host.register_animation(c.self->handle, form_of(c.args[0]), text_of(c, c.args[1]), false);
        });
    }

    // ---- image-space modifiers ----
    bind("Actor", "SetCriticalStage", [&g, host](NativeCall& c) {
        const int32_t stage = c.args[0].integer;
        const uint32_t self = self_form(c);
        if (stage < 0 || stage >= 5) {
            c.vm.log_error(c.stack, std::format("Actor critical stage of {} is invalid", stage));
            return;
        }
        game::Combat& combat = g.combat();
        combat.fighter(self).critical_stage = uint8_t(stage);
        if (stage == 2 || stage == 4) { // GooEnd, DisintegrateEnd: the 3D goes, and a dying actor finishes dying
            host.critical_end(self);
        }
    });
    bind("Actor", "AttachAshPile", [&g, host](NativeCall& c) {
        const uint32_t self = self_form(c);
        const uint32_t chosen = form_of(c.args[0]);
        uint32_t base = chosen ? chosen : 0x1Bu; // DefaultAshPile1
        if (const forms::FormEntry* e = g.db().find(base); e && e->type == forms::sig("LVLI")) {
            std::vector<game::Rolled> rolled;
            g.leveled().roll(base, 1, 1, rolled);
            base = rolled.empty() ? 0x1Bu : rolled.front().form;
        }
        const auto at = g.ref_position(self);
        if (const uint32_t pile = at ? host.place(base, *at) : 0) {
            g.link_ash_pile(pile, self);
        }
    });
    bind("ObjectReference", "KnockAreaEffect", [&g](NativeCall& c) {
        const uint32_t self = self_form(c);
        const auto from = g.ref_position(self);
        const float magnitude = float(c.args[0].real), radius = float(c.args[1].real);
        if (!from || radius <= 0.0f) {
            return;
        }
        game::Combat& combat = g.combat();
        std::vector<uint32_t> everyone = combat.world().high_actors();
        everyone.push_back(k_player_ref);
        for (const uint32_t ref : everyone) {
            const auto at = ref == self ? std::nullopt : g.ref_position(ref);
            if (!at) {
                continue;
            }
            const float dx = (*at)[0] - (*from)[0], dy = (*at)[1] - (*from)[1], dz = (*at)[2] - (*from)[2];
            const float d2 = dx * dx + dy * dy + dz * dz, r2 = radius * radius;
            if (d2 < r2) {
                combat.stagger(ref, magnitude * (r2 - d2) / r2, 0);
            }
        }
    });
    bind("ObjectReference", "PushActorAway", [&g](NativeCall& c) {
        const uint32_t victim = form_of(c.args[0]);
        const auto from = g.ref_position(self_form(c));
        if (!victim) {
            c.vm.log_error(c.stack, "Cannot push back a None actor");
            return;
        }
        if (from) {
            g.combat().world().knockdown(victim, *from, float(c.args[1].real));
        }
    });
    bind("ObjectReference", "SendStealAlarm", [&g](NativeCall& c) {
        const uint32_t thief = form_of(c.args[0]), self = self_form(c);
        if (!thief) {
            c.vm.log_error(c.stack, std::format("{:08X} cannot be stolen by a None actor", self));
            return;
        }
        // A teammate's counts as the player's; anyone else's is dropped.
        if (thief == k_player_ref || std::ranges::contains(g.ai().followers(k_player_ref), thief)) {
            g.crime().report_theft(self, g.base_of(self), 1, 0, g.owner_of(self), false);
        }
    });
    bind("Weapon", "Fire", [&g](NativeCall& c) {
        const uint32_t source = form_of(c.args[0]);
        const game::Item* weapon = g.items().find(self_form(c));
        if (!source) {
            c.vm.log_error(c.stack, "Cannot fire a weapon from a None source");
            return;
        }
        if (weapon) {
            g.projectiles().fire_weapon(source, weapon, g.items().find(form_of(c.args[1])));
        }
    });
    bind("Actor", "SetHeadTracking", [host](NativeCall& c) {
        host.graph_variable(self_form(c), "bHeadTracking", c.args[0].boolean ? 1.0f : 0.0f);
    });
    const auto register_los = [host](NativeCall& c, int type) {
        const uint32_t viewer = form_of(c.args[0]), target = form_of(c.args[1]);
        if (!viewer) {
            c.vm.log_error(c.stack, "Cannot register for LOS from a None actor");
        } else if (!target) {
            c.vm.log_error(c.stack, "Cannot register for LOS for a None target");
        } else if (c.self) {
            host.register_los(c.self->handle, viewer, target, type);
        }
    };
    for (const char* cls : {"Form", "Alias", "ActiveMagicEffect"}) {
        bind(cls, "RegisterForSingleLOSGain", [register_los](NativeCall& c) { register_los(c, 0); });
        bind(cls, "RegisterForSingleLOSLost", [register_los](NativeCall& c) { register_los(c, 1); });
        bind(cls, "RegisterForLOS", [register_los](NativeCall& c) { register_los(c, 2); });
        bind(cls, "UnregisterForLOS", [host](NativeCall& c) {
            if (c.self) {
                host.unregister_los(c.self->handle, form_of(c.args[0]), form_of(c.args[1]));
            }
        });
    }
    bind("Game", "ShowTrainingMenu", [host](NativeCall& c) { host.open_training(form_of(c.args[0])); });
    bind("Actor", "ShowGiftMenu", [host](NativeCall& c) {
        script::VirtualMachine* vm = &c.vm;
        const script::StackId stack = c.stack;
        host.open_gift(self_form(c), c.args[0].boolean, form_of(c.args[1]), c.args[2].boolean,
                       [vm, stack](int total) { vm->return_from_latent(stack, Value::of(int32_t(total))); });
    });
    bind("Message", "ShowAsHelpMessage", [&g, host](NativeCall& c) {
        if (const forms::FormEntry* e = g.db().find(self_form(c))) {
            host.help_message(std::string(text_of(c, c.args[0])), std::string(forms::Form(g.db(), *e).description()),
                              float(c.args[1].real), float(c.args[2].real), c.args[3].integer);
        }
    });
    bind("Message", "ResetHelpMessage", [host](NativeCall& c) { host.help_reset(std::string(text_of(c, c.args[0]))); });
    bind("Game", "ShakeCamera", [host](NativeCall& c) { host.shake_camera(form_of(c.args[0]), float(c.args[1].real), float(c.args[2].real)); });
    bind("ImageSpaceModifier", "Apply", [&g, host](NativeCall& c) { host.apply_modifier(editor_id(g, self_form(c)), c.args[0].real, 0.0f); });
    bind("ImageSpaceModifier", "Remove", [&g, host](NativeCall& c) { host.remove_modifier(editor_id(g, self_form(c)), 0.0f); });
    bind("ImageSpaceModifier", "PopTo", [&g, host](NativeCall& c) {
        host.remove_modifier(editor_id(g, self_form(c)), 0.0f);
        if (const uint32_t next = form_of(c.args[0])) {
            host.apply_modifier(editor_id(g, next), c.args[1].real, 0.0f);
        }
    });
    // One slot: the last cross-fade fades out as the next comes in.
    const auto crossfade = std::make_shared<std::string>();
    bind("ImageSpaceModifier", "ApplyCrossFade", [&g, host, crossfade](NativeCall& c) {
        if (!crossfade->empty()) {
            host.remove_modifier(*crossfade, c.args[0].real);
        }
        *crossfade = editor_id(g, self_form(c));
        host.apply_modifier(*crossfade, 1.0f, c.args[0].real);
    });
    bind("ImageSpaceModifier", "RemoveCrossFade", [host, crossfade](NativeCall& c) {
        if (!crossfade->empty()) {
            host.remove_modifier(*crossfade, c.args[0].real);
            crossfade->clear();
        }
    });

    // ---- sounds ----
    bind("Sound", "StopInstance", [host](NativeCall& c) {
        if (c.args[0].integer == 0) {
            c.vm.log_error(c.stack, "Attempting to stop an invalid sound instance");
            return;
        }
        host.stop_sound(uint32_t(c.args[0].integer));
    });

    // ---- weather ----
    bind("Weather", "ForceActive", [host](NativeCall& c) { host.force_weather(self_form(c)); });
    bind("Weather", "ReleaseOverride", [host](NativeCall&) { host.release_weather(); });

    // ---- the camera, fades ----
    bind("Game", "ForceFirstPerson", [host](NativeCall&) { host.force_view(true); });
    bind("Game", "ForceThirdPerson", [host](NativeCall&) { host.force_view(false); });
    bind("Game", "ShowFirstPersonGeometry", [host](NativeCall& c) { host.first_person_geometry(c.args[0].boolean); });
    bind("Game", "ShakeController", [](NativeCall&) {}); // the rumble motors: no gamepad output yet
    bind("Game", "FadeOutGame", [host](NativeCall& c) {
        host.fade(c.args[0].boolean, c.args[1].boolean, c.args[2].real, c.args[3].real);
    });

    // ---- menus ----
    bind("Actor", "ShowBarterMenu", [&g, host](NativeCall& c) { host.open_barter(g.base_of(self_form(c))); });
    bind("Actor", "OpenInventory", [&g, host](NativeCall& c) {
        const uint32_t self = self_form(c);
        if (g.ref_flag(self, game::ref_teammate) || c.args[0].boolean) {
            host.open_inventory(self);
        }
    });

    // ---- words of power ----
    bind("Game", "UnlockWord", [&g](NativeCall& c) {
        const uint32_t word = form_of(c.args[0]);
        if (!word) {
            c.vm.log_error(c.stack, "Cannot unlock a None word of power");
            return;
        }
        g.teach_word(word);
        g.casters().unlock_word(word);
    });
    bind("Game", "TeachWord", [&g](NativeCall& c) {
        const uint32_t word = form_of(c.args[0]);
        if (!word) {
            c.vm.log_error(c.stack, "Cannot teach the player a None word of power");
            return;
        }
        g.teach_word(word); // known, not unlocked
    });
    bind("Game", "IsWordUnlocked", [&g](NativeCall& c) {
        const uint32_t word = form_of(c.args[0]);
        if (!word) {
            c.vm.log_error(c.stack, "Cannot determine Locked status of a None word of power");
            c.result = Value::of(false);
            return;
        }
        c.result = Value::of(g.casters().word_unlocked(word));
    });
}

} // namespace spud::engine
