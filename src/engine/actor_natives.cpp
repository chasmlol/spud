// Papyrus natives over actors and factions: membership and rank, relationships, what the AI runs, and the state flags
// scripts set.

#include "engine/game_natives.hpp"

#include "core/units.hpp"
#include "formats/forms_typed.hpp"
#include "game/game.hpp"
#include "script/natives.hpp"
#include "script/vm.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>

namespace spud::engine {

namespace {

using game::k_not_in_faction;
using game::k_player_ref;
using script::NativeCall;
using script::Tag;
using script::Value;

uint32_t form_of(const Value& v) {
    return v.tag == Tag::object && v.object ? script::handle_form(v.object->handle) : 0;
}

uint32_t self_form(const NativeCall& c) { return c.self ? script::handle_form(c.self->handle) : 0; }

// ACBS flags ActorBase's natives set (xEdit wbDefinitionsTES5 NPC_).
constexpr uint32_t k_acbs_essential = 0x2, k_acbs_protected = 0x800, k_acbs_invulnerable = 0x80000000;

} // namespace

void bind_actor_natives(script::VirtualMachine& vm, game::Game& g, const ReferenceHost& host) {
    const auto actor = [&vm](const char* name, script::NativeFunction fn) {
        vm.bind_native("Actor", name, std::move(fn), script::native_flags("Actor", name));
    };
    const auto faction = [&vm](const char* name, script::NativeFunction fn) {
        vm.bind_native("Faction", name, std::move(fn), script::native_flags("Faction", name));
    };
    const auto base = [&vm](const char* name, script::NativeFunction fn) {
        vm.bind_native("ActorBase", name, std::move(fn), script::native_flags("ActorBase", name));
    };

    // ---- factions ----
    actor("IsInFaction", [&g](NativeCall& c) {
        c.result = Value::of(g.faction_rank(self_form(c), form_of(c.args[0])) >= 0);
    });
    actor("GetFactionRank", [&g](NativeCall& c) {
        const uint32_t f = form_of(c.args[0]);
        if (!f) {
            c.vm.log_error(c.stack, "Cannot get the faction rank with a None faction");
            return;
        }
        c.result = Value::of(g.faction_rank(self_form(c), f));
    });
    actor("SetFactionRank", [&g](NativeCall& c) {
        const uint32_t f = form_of(c.args[0]);
        if (!f) {
            c.vm.log_error(c.stack, "Cannot set the faction rank with a None faction");
            return;
        }
        g.set_faction_rank(self_form(c), f, c.args[1].integer);
    });
    actor("ModFactionRank", [&g](NativeCall& c) {
        const uint32_t self = self_form(c), f = form_of(c.args[0]);
        if (!f) {
            c.vm.log_error(c.stack, "Cannot modify the faction rank with a None faction");
            return;
        }
        if (const int32_t rank = g.faction_rank(self, f); rank >= 0) { // never adds
            g.set_faction_rank(self, f, std::max(0, rank + int8_t(c.args[1].integer)));
        }
    });
    actor("RemoveFromFaction", [&g](NativeCall& c) {
        if (const uint32_t f = form_of(c.args[0])) {
            g.leave_faction(self_form(c), f);
        }
    });
    actor("RemoveFromAllFactions", [&g](NativeCall& c) { g.leave_all_factions(self_form(c)); });
    actor("GetCrimeFaction", [&g](NativeCall& c) { c.result = c.vm.form_value(g.crime_faction(self_form(c)), "Faction"); });
    actor("SetCrimeFaction", [&g](NativeCall& c) {
        const uint32_t self = self_form(c), f = form_of(c.args[0]);
        if (f) {
            const game::CrimeFaction* crime = g.crime().faction(f);
            if (!crime || !(crime->flags & game::faction_tracks_crime)) {
                return;
            }
            if (g.faction_rank(self, f) < 0) {
                g.set_faction_rank(self, f, 1);
            }
        }
        g.changes(self).crime_faction = f; // None stands too, over the record's
    });

    faction("GetCrimeGold", [&g](NativeCall& c) { c.result = Value::of(g.crime().crime_gold(self_form(c))); });
    faction("SetCrimeGold", [&g](NativeCall& c) { g.crime().set_crime_gold(self_form(c), false, c.args[0].integer); });
    faction("SetCrimeGoldViolent", [&g](NativeCall& c) { g.crime().set_crime_gold(self_form(c), true, c.args[0].integer); });
    faction("ModCrimeGold", [&g](NativeCall& c) { g.crime().mod_crime_gold(self_form(c), c.args[1].boolean, c.args[0].integer); });
    // Latent: the script goes on once the player is on the way, the move itself is the next frame's.
    vm.bind_native("Faction", "SendPlayerToJail", [&g](NativeCall& c) {
        g.crime().go_to_jail(self_form(c), c.args[0].boolean, c.args[1].boolean);
        c.vm.return_from_latent(c.stack, {});
    }, script::native_flags("Faction", "SendPlayerToJail"));
    faction("PlayerPayCrimeGold", [&g](NativeCall& c) {
        g.crime().pay_fine(self_form(c), c.args[1].boolean, c.args[0].boolean);
    });
    faction("GetStolenItemValueNoCrime", [&g](NativeCall& c) {
        c.result = Value::of(g.crime().stolen_value(self_form(c), false));
    });
    faction("GetStolenItemValueCrime", [&g](NativeCall& c) {
        c.result = Value::of(g.crime().stolen_value(self_form(c), true));
    });
    faction("SetPlayerEnemy", [&g](NativeCall& c) { g.crime().set_enemy(self_form(c), c.args[0].boolean); });
    faction("IsPlayerEnemy", [&g](NativeCall& c) { c.result = Value::of(g.crime().is_enemy(self_form(c))); });
    faction("SetPlayerExpelled", [&g](NativeCall& c) { g.crime().set_expelled(self_form(c), c.args[0].boolean); });
    faction("IsPlayerExpelled", [&g](NativeCall& c) { c.result = Value::of(g.crime().expelled(self_form(c))); });

    // Both ways, or one: 1 enemy, 0 neutral, 2 ally, 3 friend; a neutral with no row adds nothing, which reads the same.
    const auto reactions = [&g](NativeCall& c, int self_way, int other_way, const char* none) {
        const uint32_t self = self_form(c), other = form_of(c.args[0]);
        if (!other) {
            c.vm.log_error(c.stack, none);
            return;
        }
        g.crime().set_faction_reaction(self, other, self_way);
        g.crime().set_faction_reaction(other, self, other_way);
    };
    constexpr const char* k_none_faction = "Cannot be an ally of a None faction"; // SetEnemy's text too
    faction("SetAlly", [reactions, k_none_faction](NativeCall& c) {
        reactions(c, c.args[1].boolean ? 3 : 2, c.args[2].boolean ? 3 : 2, k_none_faction);
    });
    faction("SetEnemy", [reactions, k_none_faction](NativeCall& c) {
        reactions(c, c.args[1].boolean ? 0 : 1, c.args[2].boolean ? 0 : 1, k_none_faction);
    });
    // The nearest high-process actor of the faction's base members that sees the player, alive.
    faction("SendAssaultAlarm", [&g](NativeCall& c) {
        const uint32_t self = self_form(c);
        const game::Agent* nearest = nullptr;
        const auto player = g.ref_position(k_player_ref).value_or(std::array<float, 3>{});
        float best = 0.0f;
        g.ai().for_each([&](const game::Agent& a) {
            if (!a.high || a.detection <= 0 || !g.crime().in_faction(a.npc, self) || g.actor_values(a.ref).current(24) <= 0.0f) {
                return;
            }
            const auto p = g.ref_position(a.ref).value_or(std::array<float, 3>{});
            const float d = std::hypot(p[0] - player[0], p[1] - player[1], p[2] - player[2]);
            if (!nearest || d < best) {
                nearest = &a;
                best = d;
            }
        });
        if (!nearest) {
            c.vm.log_error(c.stack, "No nearby NPC found to sound alarm, alarm aborted.", 1);
            return;
        }
        g.crime().report_attack(nearest->ref, nearest->npc);
    });
    actor("SendAssaultAlarm", [&g](NativeCall& c) {
        const uint32_t self = self_form(c);
        g.crime().report_attack(self, g.base_of(self)); // a non-guard's combat waits for Combat
    });

    // ---- relationships ----
    actor("GetRelationshipRank", [&g](NativeCall& c) {
        const uint32_t other = form_of(c.args[0]);
        if (!other) {
            c.vm.log_error(c.stack, "Cannot determine relationship rank with a None actor");
            c.result = Value::of(int32_t(0));
            return;
        }
        c.result = Value::of(int32_t(g.crime().relationship(g.base_of(self_form(c)), g.base_of(other))));
    });
    actor("SetRelationshipRank", [&g](NativeCall& c) {
        const uint32_t other = form_of(c.args[0]);
        if (!other) {
            c.vm.log_error(c.stack, "Cannot set relationship rank with a None actor");
            return;
        }
        const int32_t rank = c.args[1].integer;
        g.crime().set_relationship(g.base_of(self_form(c)), g.base_of(other), rank >= -4 && rank <= 4 ? rank : 0);
    });
    actor("HasAssociation", [&g](NativeCall& c) {
        const uint32_t type = form_of(c.args[0]);
        if (!type) {
            c.vm.log_error(c.stack, "Cannot check if an actor has a None association type");
            c.result = Value::of(false);
            return;
        }
        const uint32_t other = form_of(c.args[1]);
        c.result = Value::of(g.has_association(g.base_of(self_form(c)), type, other ? g.base_of(other) : 0));
    });
    actor("SetPlayerTeammate", [&g](NativeCall& c) {
        const uint32_t self = self_form(c);
        if (self != k_player_ref) {
            g.set_ref_flag(self, game::ref_teammate, c.args[0].boolean);
            g.set_ref_flag(self, game::ref_can_do_favor, c.args[1].boolean);
        }
    });
    actor("IsPlayerTeammate", [&g](NativeCall& c) { c.result = Value::of(g.ref_flag(self_form(c), game::ref_teammate)); });

    // ---- what the AI runs ----
    // At the next AI update, not now; not for the disabled, the dead or an actor doing a favor.
    actor("EvaluatePackage", [&g, host](NativeCall& c) {
        const uint32_t self = self_form(c);
        if (!g.ref_disabled(self) && !host.is_dead(self) && g.favor_actor() != self) {
            g.ai().evaluate(self);
        }
    });
    actor("GetCurrentPackage", [&g](NativeCall& c) {
        const game::Agent* a = g.ai().agent(self_form(c));
        c.result = c.vm.form_value(a && a->package ? a->package->form : 0, "Package");
    });
    const auto held = [&g](uint32_t ref) {
        g.ai().hold(ref, g.ref_flag(ref, game::ref_ai_off) || g.ref_flag(ref, game::ref_restrained));
    };
    vm.bind_native("ObjectReference", "PlaceActorAtMe", [&g, host](NativeCall& c) {
        const uint32_t npc = form_of(c.args[0]);
        if (!npc) {
            c.vm.log_error(c.stack, "Cannot place a None actor");
            return;
        }
        const auto at = g.ref_position(self_form(c)).value_or(std::array<float, 3>{});
        c.result = c.vm.form_value(host.place(npc, at), "Actor");
    }, script::native_flags("ObjectReference", "PlaceActorAtMe"));
    actor("EnableAI", [&g, held](NativeCall& c) {
        const uint32_t self = self_form(c);
        g.set_ref_flag(self, game::ref_ai_off, !c.args[0].boolean);
        held(self);
    });
    actor("IsAIEnabled", [&g](NativeCall& c) { c.result = Value::of(!g.ref_flag(self_form(c), game::ref_ai_off)); });
    actor("SetRestrained", [&g, held](NativeCall& c) {
        const uint32_t self = self_form(c);
        g.set_ref_flag(self, game::ref_restrained, c.args[0].boolean);
        held(self);
    });
    actor("IsRestrained", [&g](NativeCall& c) { c.result = Value::of(g.ref_flag(self_form(c), game::ref_restrained)); });
    actor("GetSitState", [&g](NativeCall& c) { c.result = Value::of(g.sit_state(self_form(c))); });
    actor("GetSleepState", [&g](NativeCall& c) { c.result = Value::of(g.sleep_state(self_form(c))); });
    actor("HasLOS", [&g](NativeCall& c) {
        const uint32_t self = self_form(c), other = form_of(c.args[0]);
        if (!other) {
            c.vm.log_error(c.stack, "Cannot request LOS to a None object");
            c.result = Value::of(false);
            return;
        }
        // Only each actor's sight of the player is known so far: the player's of it goes by the same ray.
        const game::Agent* a = g.ai().agent(self == k_player_ref ? other : self);
        c.result = Value::of(a && (self == k_player_ref || other == k_player_ref) && a->los);
    });
    actor("IsDetectedBy", [&g](NativeCall& c) {
        const uint32_t other = form_of(c.args[0]);
        if (!other) {
            c.vm.log_error(c.stack, "Cannot be detected by a None actor");
            c.result = Value::of(false);
            return;
        }
        const game::Agent* a = g.ai().agent(other);
        c.result = Value::of(a && self_form(c) == k_player_ref && a->detection > 0);
    });
    actor("IsWeaponDrawn", [host](NativeCall& c) { c.result = Value::of(host.weapon_drawn(self_form(c))); });
    actor("GetDialogueTarget", [&g](NativeCall& c) {
        const uint32_t self = self_form(c), speaker = g.dialogue_speaker();
        const uint32_t target = self == k_player_ref ? speaker : self == speaker ? k_player_ref : 0;
        c.result = c.vm.form_value(target, "Actor");
    });
    actor("MoveToPackageLocation", [&g, host](NativeCall& c) {
        const uint32_t self = self_form(c);
        if (const game::Agent* a = g.ai().agent(self); a && a->location.valid()) {
            const auto& p = a->location.point;
            g.ai().place_at(self, a->location.space, p, a->heading);
            g.move_ref(self, p);
            host.moved(self);
        }
        c.vm.return_from_latent(c.stack, {});
    });
    actor("Resurrect", [&g, host](NativeCall& c) {
        const uint32_t self = self_form(c);
        if (host.resurrect(self)) {
            g.reset_actor(self);
            g.combat().fighter(self).life = game::LifeState::alive;
            game::ActorValues& v = g.actor_values(self);
            v.restore(game::av_health, -v.modifier(game::av_health, game::Modifier::damage));
            g.ai().hold(self, false);
        } else {
            c.vm.log_error(c.stack, std::format("{:08X} is dead from the editor and cannot be resurrected", self));
        }
        c.vm.return_from_latent(c.stack, {});
    });
    const auto outfit = [&g](NativeCall& c, uint32_t npc) {
        const uint32_t o = form_of(c.args[0]);
        if (!o) {
            c.vm.log_error(c.stack, "Cannot set sleep or default outfit to None");
            return false;
        }
        const bool changed = g.base_outfit(npc, c.args[1].boolean) != o;
        g.set_base_outfit(npc, c.args[1].boolean, o);
        return changed;
    };
    actor("SetOutfit", [&g, host, outfit](NativeCall& c) {
        const uint32_t self = self_form(c);
        if (outfit(c, g.base_of(self))) {
            host.redress(self);
        }
    });
    base("SetOutfit", [outfit](NativeCall& c) { outfit(c, self_form(c)); });

    // ---- flags scripts set and read ----
    const auto flag = [&](const char* name, game::RefFlag bit) {
        actor(name, [&g, bit](NativeCall& c) {
            g.set_ref_flag(self_form(c), bit, c.args.empty() || c.args[0].boolean);
        });
    };
    actor("SetGhost", [&g](NativeCall& c) { // the reference's flag, and what magic and blows ask
        g.set_ghost(self_form(c), c.args[0].boolean);
        g.magic_target(self_form(c)).set_ghost(c.args[0].boolean);
    });
    actor("IsGhost", [&g](NativeCall& c) { c.result = Value::of(g.is_ghost(self_form(c)) || g.magic_target(self_form(c)).ghost()); });
    flag("SetAlert", game::ref_alert);
    actor("IsAlerted", [&g](NativeCall& c) { c.result = Value::of(g.ref_flag(self_form(c), game::ref_alert)); });
    // One actor at a time; ending it lets its packages run again. The lines and the HUD mode aren't built yet.
    actor("SetDoingFavor", [&g, host](NativeCall& c) {
        const uint32_t self = self_form(c);
        if (c.args[0].boolean) {
            g.set_favor_actor(self);
            if (host.say_favor) {
                host.say_favor(self, 15); // Show (4, 15)
            }
        } else if (g.favor_actor() == self) {
            g.set_favor_actor(0);
            if (host.say_favor && !g.combat().fighter(self).in_combat) {
                host.say_favor(self, 18); // ExitFavorState (4, 18)
            }
            g.ai().evaluate(self);
        }
    });
    actor("IsAllowedToFly", [&g](NativeCall& c) {
        const game::Agent* a = g.ai().agent(self_form(c));
        c.result = Value::of(a && a->allow_flying);
    });
    actor("SetForcedLandingMarker", [&g](NativeCall& c) { // None clears it
        game::Agent* a = g.ai().mutable_agent(self_form(c));
        if (!a) {
            return;
        }
        a->landing_marker = form_of(c.args[0]);
        if (const auto at = a->landing_marker ? g.ref_position(a->landing_marker) : std::nullopt) {
            a->forced_landing.position = {(*at)[0], (*at)[1], (*at)[2]};
        }
    });
    actor("GetForcedLandingMarker", [&g](NativeCall& c) {
        const game::Agent* a = g.ai().agent(self_form(c));
        c.result = c.vm.form_value(a ? a->landing_marker : 0, "ObjectReference");
    });
    actor("GetVoiceRecoveryTime", [&g](NativeCall& c) { c.result = Value::of(g.casters().voice_recovery(self_form(c))); });
    actor("SetVoiceRecoveryTime", [&g](NativeCall& c) { g.casters().set_voice_recovery(self_form(c), float(c.args[0].real)); });
    actor("IsCommandedActor", [&g](NativeCall& c) { c.result = Value::of(g.magic_target(self_form(c)).commander != 0); });
    vm.bind_native("ObjectReference", "CreateDetectionEvent", [&g, host](NativeCall& c) {
        const uint32_t owner = form_of(c.args[0]);
        if (!owner) {
            c.vm.log_error(c.stack, "Cannot generate a detection event with a None owner", 1);
            return;
        }
        game::CombatBody body;
        if (owner == k_player_ref && g.combat().world().body(self_form(c), body)) {
            g.player_detection_event() = {int(c.args[1].integer), {body.position[0], body.position[1], body.position[2]},
                                          g.settings().number("fActorAlertSoundTimer", 5.0f)};
        }
    }, script::native_flags("ObjectReference", "CreateDetectionEvent"));
    vm.bind_native("ObjectReference", "CalculateEncounterLevel", [&g](NativeCall& c) {
        // The reference's leveled level is taken as the player's: the encounter zone's limits aren't read here.
        constexpr std::array<std::pair<const char*, float>, 4> k_mults = {{{"fLeveledActorMultEasy", 0.33f}, {"fLeveledActorMultMedium", 0.67f},
                                                                          {"fLeveledActorMultHard", 1.0f}, {"fLeveledActorMultVeryHard", 1.25f}}};
        const auto difficulty = uint32_t(c.args[0].integer);
        const float mult = difficulty < 4 ? g.settings().number(k_mults[difficulty].first, k_mults[difficulty].second) : 1.0f;
        c.result = Value::of(int32_t(std::max(std::floor(float(g.character().level()) * mult), 1.0f)));
    }, script::native_flags("ObjectReference", "CalculateEncounterLevel"));
    actor("IsDoingFavor", [&g](NativeCall& c) { c.result = Value::of(g.favor_actor() == self_form(c)); });
    flag("SetNoBleedoutRecovery", game::ref_no_bleedout_recovery);
    actor("GetNoBleedoutRecovery", [&g](NativeCall& c) {
        c.result = Value::of(g.ref_flag(self_form(c), game::ref_no_bleedout_recovery));
    });
    flag("SetNotShowOnStealthMeter", game::ref_off_stealth_meter);
    actor("AllowPCDialogue", [&g](NativeCall& c) { g.set_ref_flag(self_form(c), game::ref_no_dialogue, !c.args[0].boolean); });
    flag("AllowBleedoutDialogue", game::ref_bleedout_dialogue);
    actor("SetAlpha", [&g](NativeCall& c) { g.changes(self_form(c)).alpha = std::clamp(c.args[0].real, 0.0f, 1.0f); });
    actor("SetPlayerResistingArrest", [&g](NativeCall& c) {
        g.crime().set_resisting_arrest(g.crime_faction(self_form(c)));
    });
    actor("IsTrespassing", [&g](NativeCall& c) { c.result = Value::of(self_form(c) == k_player_ref && g.crime().trespassing()); });
    actor("IsEssential", [&g](NativeCall& c) { c.result = Value::of(g.base_flag(g.base_of(self_form(c)), k_acbs_essential)); });

    base("IsEssential", [&g](NativeCall& c) { c.result = Value::of(g.base_flag(self_form(c), k_acbs_essential)); });
    base("IsProtected", [&g](NativeCall& c) { c.result = Value::of(g.base_flag(self_form(c), k_acbs_protected)); });
    base("IsInvulnerable", [&g](NativeCall& c) { c.result = Value::of(g.base_flag(self_form(c), k_acbs_invulnerable)); });
    // One clears the other.
    base("SetEssential", [&g](NativeCall& c) {
        g.set_base_flag(self_form(c), k_acbs_essential, c.args[0].boolean);
        if (c.args[0].boolean) {
            g.set_base_flag(self_form(c), k_acbs_protected, false);
        }
    });
    base("SetProtected", [&g](NativeCall& c) {
        g.set_base_flag(self_form(c), k_acbs_protected, c.args[0].boolean);
        if (c.args[0].boolean) {
            g.set_base_flag(self_form(c), k_acbs_essential, false);
        }
    });
    base("GetDeadCount", [&g](NativeCall& c) { c.result = Value::of(g.dead_count(self_form(c))); });
    base("SetInvulnerable", [&g](NativeCall& c) { g.set_base_flag(self_form(c), k_acbs_invulnerable, c.args[0].boolean); });
}

} // namespace spud::engine
