// Papyrus natives over the character (src/game/character.*): actor values, spells, perks, races, skills, and
// Message.Show, which the standing stones wait on.

#include "engine/game_natives.hpp"

#include "core/log.hpp"
#include "formats/forms_typed.hpp"
#include "game/game.hpp"
#include "game/quests.hpp"
#include "script/natives.hpp"
#include "script/vm.hpp"
#include "ui/ui.hpp"

#include <algorithm>
#include <cmath>
#include <array>
#include <format>
#include <memory>

namespace spud::engine {

namespace {

using game::k_player_ref;
using game::Modifier;
using script::NativeCall;
using script::Tag;
using script::Value;

uint32_t form_of(const Value& v) {
    return v.tag == Tag::object && v.object ? script::handle_form(v.object->handle) : 0;
}

uint32_t self_form(const NativeCall& c) { return c.self ? script::handle_form(c.self->handle) : 0; }

// A named actor value; scripts get a log line for a name that doesn't exist.
int value_of(NativeCall& c, const Value& name) {
    return game::actor_value_by_name(name.tag == Tag::string ? c.vm.text(name.string) : std::string_view());
}

// The %.0f-style tokens of a message's text with Show's nine arguments (the game's text with the float array).
std::string format_message(std::string text, std::span<const Value> args) {
    size_t next = 0;
    for (size_t at = text.find('%'); at != std::string::npos; at = text.find('%', at)) {
        size_t end = at + 1;
        while (end < text.size() && (std::isdigit(uint8_t(text[end])) || text[end] == '.')) {
            ++end;
        }
        if (end >= text.size() || (text[end] != 'f' && text[end] != 'g') || next >= args.size()) {
            at = end;
            continue;
        }
        const std::string spec = text.substr(at + 1, end - at - 1);
        const size_t dot = spec.find('.');
        const int precision = dot == std::string::npos ? 6 : std::atoi(spec.c_str() + dot + 1);
        const std::string value = std::format("{:.{}f}", args[next++].real, precision);
        text.replace(at, end + 1 - at, value);
        at += value.size();
    }
    return text;
}

} // namespace

void bind_character_natives(script::VirtualMachine& vm, game::Game& g, ui::Ui& ui) {
    game::Character& player = g.character();
    const auto values = [&g](uint32_t ref) -> game::ActorValues& { return g.actor_values(ref); };

    // ---- actor values (Actor natives) ----
    vm.bind_native("Actor", "GetActorValue", [values](NativeCall& c) {
        const int av = value_of(c, c.args[0]);
        c.result = Value::of(av < 0 ? 0.0f : values(self_form(c)).current(av));
    });
    vm.bind_native("Actor", "GetBaseActorValue", [values](NativeCall& c) {
        const int av = value_of(c, c.args[0]);
        c.result = Value::of(av < 0 ? 0.0f : values(self_form(c)).base(av));
    });
    vm.bind_native("Actor", "GetActorValuePercentage", [values](NativeCall& c) {
        const int av = value_of(c, c.args[0]);
        c.result = Value::of(av < 0 ? 0.0f : values(self_form(c)).percent(av));
    });
    vm.bind_native("Actor", "SetActorValue", [values](NativeCall& c) {
        if (const int av = value_of(c, c.args[0]); av >= 0) {
            values(self_form(c)).set_base(av, c.args[1].real);
        }
    });
    const auto writable = [](int av) { return av >= 0 && !(game::actor_value_info(av).flags & game::av_flag_no_script); };
    vm.bind_native("Actor", "ModActorValue", [values, writable](NativeCall& c) {
        if (const int av = value_of(c, c.args[0]); writable(av)) {
            values(self_form(c)).mod(Modifier::permanent, av, c.args[1].real);
        }
    });
    vm.bind_native("Actor", "ForceActorValue", [values, writable](NativeCall& c) {
        if (const int av = value_of(c, c.args[0]); writable(av)) {
            game::ActorValues& v = values(self_form(c));
            v.mod(Modifier::permanent, av, c.args[1].real - v.current(av));
        }
    });
    vm.bind_native("Actor", "DamageActorValue", [values, writable](NativeCall& c) {
        if (const int av = value_of(c, c.args[0]); writable(av)) {
            values(self_form(c)).mod(Modifier::damage, av, -std::abs(c.args[1].real));
        }
    });
    vm.bind_native("Actor", "RestoreActorValue", [values, writable](NativeCall& c) {
        if (const int av = value_of(c, c.args[0]); writable(av)) {
            const float amount = std::abs(c.args[1].real);
            const bool inverted = (game::actor_value_info(av).flags & game::av_flag_damage_positive) != 0;
            values(self_form(c)).restore(av, inverted ? -amount : amount);
        }
    });
    vm.bind_native("Actor", "GetLevel", [&g, &player](NativeCall& c) {
        const uint32_t self = self_form(c);
        c.result = Value::of(self == k_player_ref ? int32_t(player.level()) : int32_t(g.actor_values(self).record().level));
    });
    vm.bind_native("Actor", "GetRace", [&g, &player](NativeCall& c) {
        const uint32_t self = self_form(c);
        c.result = c.vm.form_value(self == k_player_ref ? player.race() : g.actor_values(self).record().race, "Race");
    });
    vm.bind_native("ActorBase", "GetRace", [&g, &player](NativeCall& c) {
        const uint32_t self = self_form(c);
        c.result = c.vm.form_value(self == game::k_player_base ? player.race() : g.actor_values(self).record().race, "Race");
    });
    vm.bind_native("Actor", "SetRace", [&player](NativeCall& c) {
        if (const uint32_t race = form_of(c.args[0]); race && self_form(c) == k_player_ref) {
            player.switch_race(race, true);
        }
    });

    // ---- spells, on any actor ----
    vm.bind_native("Actor", "AddSpell", [&g](NativeCall& c) {
        const uint32_t self = self_form(c), spell = form_of(c.args[0]);
        const bool added = spell && g.magic_target(self).add_spell(spell);
        if (added && self == k_player_ref && c.args[1].boolean) {
            const game::Spell* s = g.magic().spell(spell);
            g.notify(std::format("{} {}", s ? s->name : std::string(), g.settings().text("sAddItemtoSpellList")));
        }
        c.result = Value::of(added);
    });
    vm.bind_native("Actor", "RemoveSpell", [&g](NativeCall& c) {
        g.magic_target(self_form(c)).remove_spell(form_of(c.args[0]));
        c.result = Value::of(true);
    });
    vm.bind_native("Actor", "DispelSpell", [&g](NativeCall& c) {
        g.magic_target(self_form(c)).dispel_spell(form_of(c.args[0]));
        c.result = Value::of(true);
    });
    vm.bind_native("Actor", "DispelAllSpells", [&g](NativeCall& c) { g.magic_target(self_form(c)).dispel_all(); });
    vm.bind_native("Actor", "HasSpell", [&g](NativeCall& c) {
        c.result = Value::of(g.magic_target(self_form(c)).has_spell(form_of(c.args[0])));
    });
    vm.bind_native("Actor", "HasMagicEffect", [&g](NativeCall& c) {
        c.result = Value::of(g.magic_target(self_form(c)).has_magic_effect(form_of(c.args[0])));
    });
    vm.bind_native("Actor", "HasMagicEffectWithKeyword", [&g](NativeCall& c) {
        c.result = Value::of(g.magic_target(self_form(c)).has_magic_effect_keyword(form_of(c.args[0])));
    });
    // Latent in the game, done at once here: an instant cast has nothing to wait for.
    for (const char* item : {"Spell", "Scroll"}) {
        vm.bind_native(item, "Cast", [&g](NativeCall& c) {
            g.casters().instant_cast(form_of(c.args[0]), self_form(c), form_of(c.args[1]));
            c.vm.return_from_latent(c.stack, {}); // the cast is instant: nothing to wait for
        }, script::native_flags(item, "Cast"));
    }
    for (const char* name : {"Preload", "Unload"}) { // loads its art early; here art streams as it's needed
        vm.bind_native("Spell", name, [](NativeCall&) {});
    }
    for (const char* type : {"Spell", "Enchantment", "Scroll", "Potion", "Ingredient"}) {
        vm.bind_native(type, "IsHostile", [&g](NativeCall& c) {
            const game::Spell* s = g.magic().spell(self_form(c));
            c.result = Value::of(s && s->hostile_count > 0);
        });
    }
    vm.bind_native("Spell", "RemoteCast", [&g](NativeCall& c) {
        g.casters().instant_cast(form_of(c.args[0]), self_form(c), form_of(c.args[2]), 0, form_of(c.args[1]));
        c.vm.return_from_latent(c.stack, {}); // the cast is instant: nothing to wait for
    }, script::native_flags("Spell", "RemoteCast"));
    vm.bind_native("Actor", "DoCombatSpellApply", [&g](NativeCall& c) {
        g.magic_target(form_of(c.args[1])).combat_spell_apply(form_of(c.args[0]), self_form(c));
    });

    // ---- life and combat ----
    game::Combat& combat = g.combat();
    const auto kill = [&g, &combat](uint32_t ref, uint32_t killer, bool essential) {
        game::ActorValues& v = g.actor_values(ref);
        if (!essential && v.record().essential_or_protected() && !combat.killable(ref, killer)) {
            combat.damage_health(ref, v.current(game::av_health), killer); // stops short, as a blow would
            return;
        }
        v.killable_now = true;
        v.mod(Modifier::damage, game::av_health, -v.current(game::av_health));
        v.killable_now = false;
        combat.kill(ref, killer, essential);
    };
    vm.bind_native("Actor", "Kill", [kill](NativeCall& c) { kill(self_form(c), form_of(c.args[0]), false); });
    vm.bind_native("Actor", "KillSilent", [kill](NativeCall& c) { kill(self_form(c), form_of(c.args[0]), false); });
    vm.bind_native("Actor", "KillEssential", [kill](NativeCall& c) { kill(self_form(c), form_of(c.args[0]), true); });
    vm.bind_native("Actor", "StartCombat", [&combat](NativeCall& c) { combat.start_combat(self_form(c), form_of(c.args[0])); });
    vm.bind_native("Actor", "StopCombat", [&combat](NativeCall& c) { combat.stop_combat(self_form(c)); });
    vm.bind_native("Actor", "StopCombatAlarm", [&combat](NativeCall& c) { combat.stop_combat_alarm(self_form(c)); });
    vm.bind_native("Actor", "IsBleedingOut", [&combat](NativeCall& c) {
        const game::LifeState life = combat.fighter(self_form(c)).life;
        c.result = Value::of(life == game::LifeState::bleedout || life == game::LifeState::essential_down);
    });
    vm.bind_native("Actor", "GetCombatState", [&combat](NativeCall& c) {
        const game::Fighter& f = combat.fighter(self_form(c));
        c.result = Value::of(int32_t(!f.in_combat ? 0 : f.searching ? 2 : 1));
    });
    vm.bind_native("Actor", "IsInKillMove", [&combat](NativeCall& c) { c.result = Value::of(combat.fighter(self_form(c)).in_kill_move); });
    vm.bind_native("Actor", "SetAttackActorOnSight", [&combat](NativeCall& c) {
        combat.fighter(self_form(c)).attack_on_sight = c.args[0].boolean;
    });
    vm.bind_native("Actor", "StartDeferredKill", [&combat](NativeCall& c) {
        game::Fighter& f = combat.fighter(self_form(c));
        f.deferred_kill = std::max<uint8_t>(f.deferred_kill, 1);
    });
    vm.bind_native("Actor", "EndDeferredKill", [&combat](NativeCall& c) { combat.end_deferred_kill(self_form(c)); });
    vm.bind_native("Actor", "ResetHealthAndLimbs", [&g](NativeCall& c) {
        game::ActorValues& v = g.actor_values(self_form(c));
        if (const float damage = v.modifier(game::av_health, Modifier::damage); damage < 0.0f) {
            v.restore(game::av_health, -damage);
        }
    });
    vm.bind_native("Actor", "ClearExtraArrows", [](NativeCall&) {}); // no arrows stay in a body here
    vm.bind_native("Actor", "GetCombatTarget", [&combat](NativeCall& c) {
        c.result = c.vm.form_value(combat.fighter(self_form(c)).combat_target, "Actor");
    });
    vm.bind_native("Actor", "IsInCombat", [&combat](NativeCall& c) { c.result = Value::of(combat.fighter(self_form(c)).in_combat); });

    vm.bind_native("ObjectReference", "GetBaseObject", [&g](NativeCall& c) {
        const uint32_t self = self_form(c);
        uint32_t base = self == k_player_ref ? game::k_player_base : g.base_of(self);
        if (const forms::FormEntry* e = g.db().find(self)) {
            if (const auto name = g.db().field(*e, forms::sig("NAME"))) {
                base = name->as<uint32_t>();
            }
        }
        c.result = c.vm.form_value(base, "Form");
    });
    vm.bind_native("Actor", "GetActorBase", [&g](NativeCall& c) {
        const uint32_t self = self_form(c);
        c.result = c.vm.form_value(self == k_player_ref ? game::k_player_base : g.actor_values(self).record().npc, "ActorBase");
    });
    vm.bind_native("Actor", "GetLeveledActorBase", [&g](NativeCall& c) {
        const uint32_t self = self_form(c);
        c.result = c.vm.form_value(self == k_player_ref ? game::k_player_base : g.actor_values(self).record().npc, "ActorBase");
    });

    // ---- a running effect ----
    const auto running = [&g](NativeCall& c) -> const game::ActiveEffect* {
        const uint32_t id = c.self ? script::handle_form(c.self->handle) : 0;
        const game::MagicTarget* target = g.effect_target(id);
        if (target) {
            for (const game::ActiveEffect& e : target->effects()) {
                if (e.id == id) {
                    return &e;
                }
            }
        }
        return nullptr; // gone: by OnEffectFinish the game's answer is None too
    };
    vm.bind_native("ActiveMagicEffect", "GetTargetActor", [running](NativeCall& c) {
        const game::ActiveEffect* e = running(c);
        c.result = c.vm.form_value(e ? e->target : 0, "Actor");
    });
    vm.bind_native("ActiveMagicEffect", "GetCasterActor", [running](NativeCall& c) {
        const game::ActiveEffect* e = running(c);
        c.result = c.vm.form_value(e ? e->caster : 0, "Actor");
    });
    vm.bind_native("ActiveMagicEffect", "GetBaseObject", [running](NativeCall& c) {
        const game::ActiveEffect* e = running(c);
        c.result = c.vm.form_value(e ? e->effect : 0, "MagicEffect");
    });
    vm.bind_native("ActiveMagicEffect", "GetMagnitude", [running](NativeCall& c) {
        const game::ActiveEffect* e = running(c);
        c.result = Value::of(e ? e->magnitude : 0.0f);
    });
    vm.bind_native("ActiveMagicEffect", "GetDuration", [running](NativeCall& c) {
        const game::ActiveEffect* e = running(c);
        c.result = Value::of(e ? e->duration : 0.0f);
    });
    vm.bind_native("ActiveMagicEffect", "GetTimeElapsed", [running](NativeCall& c) {
        const game::ActiveEffect* e = running(c);
        c.result = Value::of(e ? e->elapsed : 0.0f);
    });
    vm.bind_native("ActiveMagicEffect", "Dispel", [&g](NativeCall& c) {
        const uint32_t id = c.self ? script::handle_form(c.self->handle) : 0;
        if (game::MagicTarget* target = g.effect_target(id)) {
            target->dispel_effect(id);
        }
    });

    // ---- the controls scripts ask after (who reads them is the input's side) ----
    const auto controls = std::make_shared<std::array<bool, 8>>();
    controls->fill(true);
    static constexpr const char* k_controls[8] = {"IsMovementControlsEnabled", "IsFightingControlsEnabled", "IsCamSwitchControlsEnabled",
                                                  "IsLookingControlsEnabled", "IsSneakingControlsEnabled", "IsMenuControlsEnabled",
                                                  "IsActivateControlsEnabled", "IsJournalControlsEnabled"};
    for (size_t i = 0; i < 8; ++i) {
        vm.bind_native("Game", k_controls[i], [controls, i](NativeCall& c) { c.result = Value::of((*controls)[i]); });
    }
    const auto set_controls = [controls](NativeCall& c, bool on) {
        for (size_t i = 0; i < 8 && i < c.args.size(); ++i) {
            if (c.args[i].boolean) {
                (*controls)[i] = on;
            }
        }
    };
    vm.bind_native("Game", "DisablePlayerControls", [set_controls, &ui](NativeCall& c) {
        // abMenu: what the player opened goes. It's what shuts a Black Book as the warp begins.
        if (c.args.size() > 5 && c.args[5].boolean) {
            for (const char* menu : {"Book Menu", "InventoryMenu", "MagicMenu", "MapMenu", "StatsMenu", "TweenMenu"}) {
                ui.close(menu);
            }
        }
        set_controls(c, false);
    });
    vm.bind_native("Game", "EnablePlayerControls", [set_controls](NativeCall& c) { set_controls(c, true); });

    // ---- equipped spells and shouts: what the Magic Menu and scripts agree on until casting is Combat's ----
    const auto spells = std::make_shared<std::array<uint32_t, 3>>();
    const auto shouts = std::make_shared<std::vector<uint32_t>>();
    const auto shout = std::make_shared<uint32_t>(0);
    vm.bind_native("Actor", "GetEquippedSpell", [spells](NativeCall& c) {
        const auto slot = size_t(std::clamp(c.args[0].integer, 0, 2));
        c.result = c.vm.form_value((*spells)[slot], "Spell");
    });
    vm.bind_native("Actor", "EquipSpell", [spells, &g](NativeCall& c) {
        (*spells)[size_t(std::clamp(c.args[1].integer, 0, 2))] = form_of(c.args[0]);
        g.casters().select(self_form(c), uint8_t(std::clamp(c.args[1].integer, 0, 2)), form_of(c.args[0]));
    });
    vm.bind_native("Actor", "UnequipSpell", [spells](NativeCall& c) {
        uint32_t& slot = (*spells)[size_t(std::clamp(c.args[1].integer, 0, 2))];
        if (slot == form_of(c.args[0])) {
            slot = 0;
        }
    });
    vm.bind_native("Actor", "AddShout", [shouts, &g](NativeCall& c) { // any actor: Actor
        const uint32_t s = form_of(c.args[0]), self = self_form(c);
        auto& list = self == k_player_ref ? *shouts : g.magic_target(self).added_spells();
        const bool added = std::find(list.begin(), list.end(), s) == list.end();
        if (added) {
            list.push_back(s);
        }
        c.result = Value::of(added);
    });
    vm.bind_native("Actor", "RemoveShout", [shouts](NativeCall& c) { c.result = Value::of(std::erase(*shouts, form_of(c.args[0])) > 0); });
    // Papyrus_Actor_DrawWeapon: the actor's own draw (vtable slot 166), which the Ready Weapon key also reaches.
    const auto ready = [&g](NativeCall& c, bool drawn) {
        game::Fighter& f = g.combat().fighter(self_form(c));
        if (f.weapon_drawn != drawn) {
            f.weapon_drawn = drawn;
            g.combat().world().perform_action(f.ref, drawn ? "ActionDraw" : "ActionSheath");
        }
    };
    vm.bind_native("Actor", "DrawWeapon", [ready](NativeCall& c) { ready(c, true); });
    vm.bind_native("Actor", "SheatheWeapon", [ready](NativeCall& c) { ready(c, false); });
    vm.bind_native("Actor", "EquipShout", [shout, &g](NativeCall& c) { g.casters().select(self_form(c), game::source_voice, form_of(c.args[0])); *shout = form_of(c.args[0]); });
    vm.bind_native("Actor", "UnequipShout", [shout](NativeCall& c) {
        if (*shout == form_of(c.args[0])) {
            *shout = 0;
        }
    });
    vm.bind_native("Actor", "GetEquippedShout", [shout](NativeCall& c) { c.result = c.vm.form_value(*shout, "Shout"); });

    // ---- perks: nothing on an NPC (Actor::AddPerk is empty) ----
    vm.bind_native("Actor", "AddPerk", [&player](NativeCall& c) {
        if (self_form(c) == k_player_ref) {
            player.add_perk(form_of(c.args[0]));
        }
    });
    vm.bind_native("Actor", "RemovePerk", [&player](NativeCall& c) {
        if (self_form(c) == k_player_ref) {
            player.remove_perk(form_of(c.args[0]));
        }
    });
    vm.bind_native("Actor", "HasPerk", [&g, &player](NativeCall& c) {
        const uint32_t self = self_form(c), perk = form_of(c.args[0]);
        c.result = Value::of(self == k_player_ref ? player.has_perk(perk) : g.conditions().value(448, self, 0, perk, 0) != 0);
    });

    // ---- the Game script ----
    vm.bind_native("Game", "AddPerkPoints", [&player](NativeCall& c) { player.add_perk_points(c.args[0].integer); });
    const auto skill_of = [](NativeCall& c, const Value& name) {
        const int av = value_of(c, name);
        return av >= game::k_first_skill_value && av < game::k_first_skill_value + game::k_skill_value_count ? av : -1;
    };
    vm.bind_native("Game", "AdvanceSkill", [&player, skill_of](NativeCall& c) {
        if (const int av = skill_of(c, c.args[0]); av >= 0 && c.args[1].real > 0.0f) {
            player.use_skill(av, c.args[1].real);
        }
    });
    vm.bind_native("Game", "IncrementSkill", [&player, skill_of](NativeCall& c) {
        if (const int av = skill_of(c, c.args[0]); av >= 0) {
            player.increment_skill(av, 1);
        }
    });
    vm.bind_native("Game", "IncrementSkillBy", [&player, skill_of](NativeCall& c) {
        if (const int av = skill_of(c, c.args[0]); av >= 0) {
            player.increment_skill(av, c.args[1].integer);
        }
    });
    vm.bind_native("Game", "SetBeastForm", [&player](NativeCall& c) { player.set_beast_form(c.args[0].boolean); });
    vm.bind_native("Game", "ShowRaceMenu", [&ui](NativeCall&) { ui.open("RaceSex Menu"); });
    vm.bind_native("Game", "ShowLimitedRaceMenu", [&ui](NativeCall&) { ui.open("RaceSex Menu|limited"); });
    // SKSE's, over the same data.
    vm.bind_native("Game", "GetPerkPoints", [&player](NativeCall& c) { c.result = Value::of(int32_t(player.perk_points())); });
    vm.bind_native("Game", "GetPlayerExperience", [&player](NativeCall& c) { c.result = Value::of(player.experience()); });

    // ---- Message.Show (latent: the box's answer) ----
    vm.bind_native("Message", "Show", [&g, &ui](NativeCall& c) {
        const forms::FormEntry* e = g.db().find(self_form(c));
        if (!e) {
            c.vm.return_from_latent(c.stack, {}); // the cast is instant: nothing to wait for
            return;
        }
        const forms::Form f(g.db(), *e);
        std::string text = format_message(std::string(f.description()), c.args);
        // Message.Show: tags through the owner quest's current instance.
        if (const forms::Value* owner = f.get("QNAM"); owner && g.quests()) {
            if (const game::Quest* q = g.quests()->quest(owner->form)) {
                text = g.quests()->quest_text(*q, q->instance, std::move(text));
            }
        }
        const forms::Value* flags = f.get("DNAM");
        std::vector<std::string> buttons;
        for (const forms::Value* b : f.all("ITXT")) {
            buttons.push_back(b->text);
        }
        if (!flags || !(flags->integer & 1) || buttons.empty()) {
            // Not a message box: a notification, and the script goes on.
            g.notify(text);
            c.vm.return_from_latent(c.stack, Value::of(int32_t(0)));
            return;
        }
        script::VirtualMachine* machine = &c.vm;
        const script::StackId stack = c.stack;
        ui.message(text, std::move(buttons), [machine, stack](int button) {
            machine->return_from_latent(stack, Value::of(int32_t(button)));
        });
    }, script::native_flags("Message", "Show"));
}

} // namespace spud::engine
