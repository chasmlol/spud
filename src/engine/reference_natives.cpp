// Papyrus natives over placed references (ObjectReference): enable state with enable parents, linked references,
// places, locks and activation.

#include "engine/game_natives.hpp"

#include "core/log.hpp"
#include "core/units.hpp"
#include "formats/forms_typed.hpp"
#include "game/quests.hpp"
#include "game/game.hpp"
#include "script/natives.hpp"
#include "script/vm.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <memory>
#include <unordered_set>

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

// The class a reference's object goes by.
std::string_view ref_class(const forms::Database& db, uint32_t ref) {
    const forms::FormEntry* e = db.find(ref);
    return ref == k_player_ref || (e && e->type == forms::sig("ACHR")) ? "Actor" : "ObjectReference";
}

} // namespace

void bind_reference_natives(script::VirtualMachine& vm, game::Game& g, ReferenceHost host) {
    const auto latent = [&vm](const char* name, script::NativeFunction fn) {
        vm.bind_native("ObjectReference", name, std::move(fn), script::native_flags("ObjectReference", name));
    };
    const auto set = [&g](NativeCall& c, bool disabled) {
        if (!g.set_ref_disabled(self_form(c), disabled)) {
            log::warn("papyrus: cannot {} an object with an enable state parent", disabled ? "disable" : "enable");
        }
    };
    // Enable and Disable wait for the fade; spud's take the frame.
    latent("Enable", [set](NativeCall& c) {
        set(c, false);
        c.vm.return_from_latent(c.stack, {});
    });
    latent("Disable", [set](NativeCall& c) {
        set(c, true);
        c.vm.return_from_latent(c.stack, {});
    });
    latent("ApplyHavokImpulse", [host](NativeCall& c) {
        if (host.havok_impulse) {
            host.havok_impulse(self_form(c), {c.args[0].real, c.args[1].real, c.args[2].real}, c.args[3].real);
        }
        c.vm.return_from_latent(c.stack, {});
    });
    // ProcessTrapHit: nothing on a plain reference; an actor takes a hit.
    vm.bind_native("ObjectReference", "ProcessTrapHit", [host](NativeCall& c) {
        if (host.trap_hit) {
            host.trap_hit(self_form(c), form_of(c.args[0]), c.args[1].real, c.args[2].real, {c.args[3].real, c.args[4].real, c.args[5].real},
                          {c.args[6].real, c.args[7].real, c.args[8].real});
        }
    }, script::native_flags("ObjectReference", "ProcessTrapHit"));
    // Actor.SetExpressionOverride: 0 and above 16 refused, strength as a percent, capped.
    vm.bind_native("Actor", "SetExpressionOverride", [host](NativeCall& c) {
        const int mood = c.args[0].integer;
        if (host.expression_override && mood >= 1 && mood <= 16) {
            host.expression_override(self_form(c), mood, std::min(c.args[1].integer, 100) * 0.01f);
        }
    }, script::native_flags("Actor", "SetExpressionOverride"));
    vm.bind_native("Actor", "ClearExpressionOverride", [host](NativeCall& c) {
        if (host.expression_override) {
            host.expression_override(self_form(c), -1, 0.0f);
        }
    }, script::native_flags("Actor", "ClearExpressionOverride"));
    latent("DamageObject", [host](NativeCall& c) {
        if (host.damage_object) {
            host.damage_object(self_form(c), c.args[0].real);
        }
        c.vm.return_from_latent(c.stack, {});
    });
    vm.bind_native("ObjectReference", "ClearDestruction", [host](NativeCall& c) {
        if (host.clear_destruction) {
            host.clear_destruction(self_form(c));
        }
    }, script::native_flags("ObjectReference", "ClearDestruction"));
    vm.bind_native("ObjectReference", "GetCurrentDestructionStage", [host](NativeCall& c) {
        c.result = Value::of(host.destruction_stage ? host.destruction_stage(self_form(c)) : 0);
    }, script::native_flags("ObjectReference", "GetCurrentDestructionStage"));
    // Kill and its kin are the character natives': they go through Combat::kill. A corpse the world started with is dead too.
    vm.bind_native("Actor", "IsDead", [&g, host](NativeCall& c) {
        const game::Fighter& f = g.combat().fighter(self_form(c));
        c.result = Value::of(f.life != game::LifeState::reanimated && (f.dead() || (host.is_dead && host.is_dead(self_form(c)))));
    }, script::native_flags("Actor", "IsDead"));
    vm.bind_native("ObjectReference", "EnableNoWait", [set](NativeCall& c) { set(c, false); }, script::native_flags("ObjectReference", "EnableNoWait"));
    vm.bind_native("ObjectReference", "DisableNoWait", [set](NativeCall& c) { set(c, true); }, script::native_flags("ObjectReference", "DisableNoWait"));
    vm.bind_native("ObjectReference", "RemoveAllItems", [&g](NativeCall& c) {
        game::Inventory& from = g.inventory_of(self_form(c));
        const uint32_t other = form_of(c.args[0]);
        for (const game::Stack& stack : std::vector<game::Stack>(from.stacks())) {
            const int32_t taken = from.remove(stack.form, stack.count);
            if (other) {
                g.inventory_of(other).add(stack.form, taken, stack.extra);
            }
        }
    }, script::native_flags("ObjectReference", "RemoveAllItems"));
    vm.bind_native("ObjectReference", "IsDisabled", [&g](NativeCall& c) { c.result = Value::of(g.ref_disabled(self_form(c))); });

    vm.bind_native("ObjectReference", "GetLinkedRef", [&g](NativeCall& c) {
        const uint32_t linked = g.conditions().linked_ref(self_form(c), form_of(c.args[0]));
        c.result = c.vm.form_value(linked, ref_class(g.db(), linked));
    });
    vm.bind_native("ObjectReference", "GetParentCell", [&g, host](NativeCall& c) {
        const uint32_t self = self_form(c);
        const forms::FormEntry* e = g.db().find(self);
        c.result = c.vm.form_value(self == k_player_ref ? host.player_cell() : e ? e->parent : 0, "Cell");
    });
    const auto position = [&g, host](uint32_t ref) {
        return ref == k_player_ref ? host.player_position() : g.ref_position(ref).value_or(std::array<float, 3>{});
    };
    vm.bind_native("ObjectReference", "GetPositionX", [position](NativeCall& c) { c.result = Value::of(position(self_form(c))[0]); });
    vm.bind_native("ObjectReference", "GetPositionY", [position](NativeCall& c) { c.result = Value::of(position(self_form(c))[1]); });
    vm.bind_native("ObjectReference", "GetPositionZ", [position](NativeCall& c) { c.result = Value::of(position(self_form(c))[2]); });
    vm.bind_native("ObjectReference", "GetDistance", [position](NativeCall& c) {
        const auto a = position(self_form(c)), b = position(form_of(c.args[0]));
        c.result = Value::of(std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]));
    });
    vm.bind_native("ObjectReference", "Is3DLoaded", [host](NativeCall& c) { c.result = Value::of(host.loaded(self_form(c))); });
    vm.bind_native("Cell", "IsInterior", [&g](NativeCall& c) {
        const forms::FormEntry* e = g.db().find(self_form(c));
        const auto data = e ? g.db().field(*e, forms::sig("DATA")) : std::nullopt;
        c.result = Value::of(data && !data->data.empty() && (uint8_t(data->data[0]) & 1) != 0);
    });
    vm.bind_native("ObjectReference", "GetWorldSpace", [&g, host](NativeCall& c) {
        c.result = c.vm.form_value(self_form(c) == k_player_ref ? g.world().player_worldspace : 0, "WorldSpace");
    });
    vm.bind_native("ObjectReference", "HasEffectKeyword", [&g](NativeCall& c) {
        c.result = Value::of(self_form(c) == k_player_ref && g.character().has_magic_effect_keyword(form_of(c.args[0])));
    });
    latent("MoveTo", [&g, host, position](NativeCall& c) {
        const uint32_t self = self_form(c);
        auto p = position(form_of(c.args[0]));
        for (size_t i = 0; i < 3; ++i) {
            p[i] += c.args[1 + i].real;
        }
        if (self == k_player_ref) {
            // Queued, as the game's: a space can't change under a running script. The call ends when the player is there.
            host.move_player_to(form_of(c.args[0]), p, c.args[4].boolean, c.stack);
            return;
        }
        g.move_ref(self, p);
        host.moved(self);
        host.move_agent(self, form_of(c.args[0]), p);
        c.vm.return_from_latent(c.stack, {});
    });

    // ---- the Actor and Game natives Survival's clocks read ----
    vm.bind_native("Actor", "GetActorValueMax", [&g](NativeCall& c) {
        const int av = c.args[0].tag == Tag::string ? game::actor_value_by_name(c.vm.text(c.args[0].string)) : -1;
        c.result = Value::of(av < 0 ? 0.0f : g.actor_values(self_form(c)).maximum(av));
    });
    vm.bind_native("Actor", "GetWarmthRating", [&g](NativeCall& c) { c.result = Value::of(self_form(c) == k_player_ref ? g.warmth() : 0.0f); });
    vm.bind_native("Actor", "IsOverEncumbered", [&g](NativeCall& c) { c.result = Value::of(self_form(c) == k_player_ref && g.over_encumbered()); });
    vm.bind_native("ActorBase", "GetSex", [&g](NativeCall& c) {
        const uint32_t self = self_form(c);
        if (self == game::k_player_base) {
            c.result = Value::of(int32_t(g.character().female()));
        } else {
            c.result = Value::of(int32_t(g.actor_values(self).record().acbs_flags & 1));
        }
    });
    vm.bind_native("Game", "QueryStat", [&g](NativeCall& c) { c.result = Value::of(g.stat(std::string(c.vm.text(c.args[0].string)))); });
    vm.bind_native("Game", "IncrementStat", [&g](NativeCall& c) { g.increment_stat(std::string(c.vm.text(c.args[0].string)), c.args[1].integer); });
    vm.bind_native("Game", "AddAchievement", [](NativeCall&) {});

    // ---- weather ----
    vm.bind_native("Weather", "GetCurrentWeather", [host](NativeCall& c) { c.result = c.vm.form_value(host.weather(0), "Weather"); });
    vm.bind_native("Weather", "GetOutgoingWeather", [host](NativeCall& c) { c.result = c.vm.form_value(host.weather(1), "Weather"); });
    vm.bind_native("Weather", "GetCurrentWeatherTransition", [host](NativeCall& c) { c.result = Value::of(host.weather_transition()); });
    vm.bind_native("Weather", "GetClassification", [&g](NativeCall& c) {
        const forms::FormEntry* e = g.db().find(self_form(c));
        const forms::Value* flags = nullptr;
        std::optional<forms::Form> f;
        if (e) {
            f.emplace(g.db(), *e);
            flags = f->get("DATA.flags");
        }
        const int64_t bits = flags ? flags->integer : 0;
        c.result = Value::of(int32_t(bits & 1 ? 0 : bits & 2 ? 1 : bits & 4 ? 2 : bits & 8 ? 3 : -1));
    });

    // ---- leveled lists scripts add to ----
    for (const char* cls : {"LeveledItem", "LeveledActor"}) {
        vm.bind_native(cls, "AddForm", [&g](NativeCall& c) {
            g.leveled().add_form(self_form(c), form_of(c.args[0]), uint16_t(c.args[1].integer), uint16_t(c.args.size() > 2 ? c.args[2].integer : 1));
        });
        vm.bind_native(cls, "Revert", [&g](NativeCall& c) { g.leveled().revert(self_form(c)); });
    }

    // ---- equipment ----
    // An NPC's are what it's drawn with: the look its gear decides, with what scripts changed.
    const auto npc_weapon = [&g](uint32_t ref, bool left) {
        const game::Gear gear = g.combat().gear(ref);
        const game::Item* item = left ? gear.left.item : gear.right.item;
        return item ? item->form : 0u;
    };
    vm.bind_native("Actor", "IsEquipped", [&g](NativeCall& c) {
        const uint32_t self = self_form(c), form = form_of(c.args[0]);
        if (self == k_player_ref) {
            c.result = Value::of(g.is_equipped(form));
            return;
        }
        std::vector<uint32_t> worn;
        uint32_t right = 0, left = 0;
        g.combat().world().gear(self, worn, right, left);
        c.result = Value::of(form && (form == right || form == left || std::ranges::contains(worn, form)));
    });
    vm.bind_native("Actor", "EquipItem", [&g, host](NativeCall& c) {
        const uint32_t self = self_form(c), form = form_of(c.args[0]);
        game::Inventory& items = g.inventory_of(self);
        if (!items.count(form)) {
            items.add(form, 1); // EquipItem gives it when it's missing
        }
        if (self == k_player_ref) {
            g.equip_form(form, true);
        } else {
            host.npc_equip(self, form, true);
        }
    });
    vm.bind_native("Actor", "UnequipItem", [&g, host](NativeCall& c) {
        const uint32_t self = self_form(c), form = form_of(c.args[0]);
        if (self == k_player_ref) {
            g.equip_form(form, false);
        } else {
            host.npc_equip(self, form, false);
        }
    });
    vm.bind_native("Actor", "UnequipAll", [&g, host](NativeCall& c) {
        if (self_form(c) == k_player_ref) {
            g.unequip_all();
        } else {
            host.npc_bare(self_form(c));
        }
    });
    vm.bind_native("Actor", "GetEquippedWeapon", [&g, npc_weapon](NativeCall& c) {
        const uint32_t self = self_form(c);
        c.result = c.vm.form_value(self == k_player_ref ? g.equipped_weapon(c.args[0].boolean) : npc_weapon(self, c.args[0].boolean), "Weapon");
    });
    vm.bind_native("Actor", "GetEquippedItemType", [&g, npc_weapon](NativeCall& c) {
        const uint32_t self = self_form(c);
        const bool left = c.args[0].integer == 0;
        if (self == k_player_ref) {
            c.result = Value::of(int32_t(g.equipped_item_type(left)));
            return;
        }
        // A weapon's animation type, the crossbow's 9 reading 12, as the player's; a shield 10 in the left.
        const game::Gear gear = g.combat().gear(self);
        const game::Item* item = g.items().find(npc_weapon(self, left));
        c.result = Value::of(int32_t(item ? (item->animation_type == 9 ? 12 : item->animation_type) : left && gear.shield ? 10 : 0));
    });

    // ---- inventory events ----
    for (const char* cls : {"ObjectReference", "ReferenceAlias", "ActiveMagicEffect"}) {
        const uint32_t flags = script::native_flags(cls, "AddInventoryEventFilter");
        vm.bind_native(cls, "AddInventoryEventFilter", [host](NativeCall& c) { host.inventory_filter(c.self->handle, form_of(c.args[0]), 1); }, flags);
        vm.bind_native(cls, "RemoveInventoryEventFilter", [host](NativeCall& c) { host.inventory_filter(c.self->handle, form_of(c.args[0]), 0); }, flags);
        vm.bind_native(cls, "RemoveAllInventoryEventFilters", [host](NativeCall& c) { host.inventory_filter(c.self->handle, 0, -1); }, flags);
    }

    // ---- locations ----
    const auto parent_of = [&g](uint32_t location) {
        const forms::FormEntry* e = g.db().find(location);
        const auto f = e ? g.db().field(*e, forms::sig("PNAM")) : std::nullopt;
        return f ? f->as<uint32_t>() : 0u;
    };
    vm.bind_native("Location", "IsChild", [parent_of](NativeCall& c) {
        const uint32_t ancestor = self_form(c);
        bool child = false;
        for (uint32_t l = parent_of(form_of(c.args[0])); l && !child; l = parent_of(l)) {
            child = l == ancestor;
        }
        c.result = Value::of(child);
    });
    vm.bind_native("Location", "HasCommonParent", [parent_of](NativeCall& c) {
        // With a keyword the common parent must have it: the houses pass BYOHHouseLocation.
        const uint32_t a = self_form(c), b = form_of(c.args[0]);
        bool common = false;
        for (uint32_t x = a; x && !common; x = parent_of(x)) {
            for (uint32_t y = b; y && !common; y = parent_of(y)) {
                common = x == y;
            }
        }
        c.result = Value::of(common);
    });
    vm.bind_native("Location", "GetRefTypeAliveCount", [&g](NativeCall& c) { // no state counts as alive
        const uint32_t type = form_of(c.args[0]);
        if (!type) {
            c.vm.log_error(c.stack, "Cannot get a count from a None location ref type", 1);
            c.result = Value::of(int32_t(0));
            return;
        }
        int32_t alive = 0;
        if (game::QuestSystem* quests = g.quests()) {
            for (const uint32_t ref : quests->world().location_refs(self_form(c), type)) {
                const game::RefState state = quests->world().ref_state(ref);
                alive += !state.disabled && !state.dead ? 1 : 0;
            }
        }
        c.result = Value::of(alive);
    });
    vm.bind_native("ObjectReference", "DropObject", [&g, host, position](NativeCall& c) {
        const uint32_t self = self_form(c), item = form_of(c.args[0]);
        if (!item) {
            c.vm.log_error(c.stack, "Cannot drop a None item", 1);
            c.vm.return_from_latent(c.stack, c.vm.form_value(0, "ObjectReference"));
            return;
        }
        g.inventory_of(self).remove(item, std::max(int32_t(c.args[1].integer), 1));
        c.vm.return_from_latent(c.stack, c.vm.form_value(host.place(item, position(self)), "ObjectReference"));
    }, script::native_flags("ObjectReference", "DropObject"));
    vm.bind_native("Location", "SetKeywordData", [&g](NativeCall& c) { g.set_location_data(self_form(c), form_of(c.args[0]), c.args[1].real); });
    vm.bind_native("Location", "GetKeywordData", [&g](NativeCall& c) { c.result = Value::of(g.location_data(self_form(c), form_of(c.args[0]))); });
    vm.bind_native("Quest", "UpdateCurrentInstanceGlobal", [](NativeCall& c) { c.result = Value::of(true); });

    // ---- what scripts place and take away ----
    vm.bind_native("ObjectReference", "PlaceAtMe", [&g, host, position](NativeCall& c) {
        const uint32_t placed = host.place(form_of(c.args[0]), position(self_form(c)));
        c.result = c.vm.form_value(placed, "ObjectReference");
    });
    vm.bind_native("ObjectReference", "Delete", [&g, host](NativeCall& c) {
        host.remove(self_form(c));
        c.vm.return_from_latent(c.stack, {});
    }, script::native_flags("ObjectReference", "Delete"));
    const auto destroyed = std::make_shared<std::unordered_set<uint32_t>>();
    vm.bind_native("ObjectReference", "SetDestroyed", [destroyed](NativeCall& c) {
        if (c.args[0].boolean) {
            destroyed->insert(self_form(c));
        } else {
            destroyed->erase(self_form(c));
        }
    });
    vm.bind_native("ObjectReference", "IsDestroyed", [destroyed](NativeCall& c) { c.result = Value::of(destroyed->contains(self_form(c))); });

    // ---- sleep ----
    for (const char* cls : {"Form", "Alias", "ActiveMagicEffect"}) {
        vm.bind_native(cls, "RegisterForSleep", [host](NativeCall& c) { host.register_sleep(c.self->handle, true); });
        vm.bind_native(cls, "UnregisterForSleep", [host](NativeCall& c) { host.register_sleep(c.self->handle, false); });
    }
    vm.bind_native("Sound", "Play", [host](NativeCall& c) {
        c.result = Value::of(int32_t(host.play_form_sound(self_form(c))));
    });
    vm.bind_native("ObjectReference", "BlockActivation", [&g](NativeCall& c) { g.block_activation(self_form(c), c.args[0].boolean); });
    vm.bind_native("ObjectReference", "IsActivationBlocked", [&g](NativeCall& c) { c.result = Value::of(g.activation_blocked(self_form(c))); });
    vm.bind_native("ObjectReference", "Activate", [host](NativeCall& c) {
        host.activate(self_form(c), form_of(c.args[0]), c.args.size() > 1 && c.args[1].boolean);
        c.result = Value::of(true);
    });
    vm.bind_native("ObjectReference", "IsLocked", [&g](NativeCall& c) { c.result = Value::of(g.inspect(self_form(c)).locked()); });
    vm.bind_native("ObjectReference", "Lock", [&g](NativeCall& c) {
        const uint32_t self = g.lock_holder(self_form(c));
        g.changes(self).locked = c.args[0].boolean;
        if (!c.args[0].boolean) {
            g.unlock(self);
        } else {
            g.relock(self);
        }
    });
    vm.bind_native("ObjectReference", "SetLockLevel", [&g](NativeCall& c) {
        g.changes(g.lock_holder(self_form(c))).lock_level = uint8_t(std::clamp(c.args[0].integer, 0, 255));
    });

    // ---- angles (degrees to scripts), scale and places ----
    const auto angle = [&g, host](uint32_t ref) {
        return g.ref_angle(ref);
    };
    const auto degrees = [](float radians) { return radians * 180.0f / 3.14159265f; };
    vm.bind_native("ObjectReference", "GetAngleX", [angle, degrees](NativeCall& c) { c.result = Value::of(degrees(angle(self_form(c))[0])); });
    vm.bind_native("ObjectReference", "GetAngleY", [angle, degrees](NativeCall& c) { c.result = Value::of(degrees(angle(self_form(c))[1])); });
    vm.bind_native("ObjectReference", "GetAngleZ", [angle, degrees](NativeCall& c) { c.result = Value::of(degrees(angle(self_form(c))[2])); });
    latent("SetAngle", [&g, host](NativeCall& c) {
        const uint32_t self = self_form(c);
        const float k = 3.14159265f / 180.0f;
        g.changes(self).angle = std::array<float, 3>{c.args[0].real * k, c.args[1].real * k, c.args[2].real * k};
        host.moved(self);
        c.vm.return_from_latent(c.stack, {});
    });
    latent("SetPosition", [&g, host](NativeCall& c) {
        const uint32_t self = self_form(c);
        const std::array<float, 3> p{c.args[0].real, c.args[1].real, c.args[2].real};
        if (self == k_player_ref) {
            host.move_player(p);
        } else {
            g.move_ref(self, p);
            host.moved(self);
            host.move_agent(self, 0, p);
        }
        c.vm.return_from_latent(c.stack, {});
    });
    latent("MoveToMyEditorLocation", [&g, host](NativeCall& c) {
        const uint32_t self = self_form(c);
        g.restore_ref_place(self);
        host.moved(self);
        c.vm.return_from_latent(c.stack, {});
    });
    vm.bind_native("ObjectReference", "GetScale", [&g](NativeCall& c) { c.result = Value::of(g.ref_scale(self_form(c))); });
    latent("SetScale", [&g](NativeCall& c) {
        // A whole percent, 1 to 1000.
        g.changes(self_form(c)).scale = std::clamp(std::round(c.args[0].real * 100.0f), 1.0f, 1000.0f) * 0.01f;
        c.vm.return_from_latent(c.stack, {});
    });
    vm.bind_native("ObjectReference", "GetHeadingAngle", [position, angle, degrees](NativeCall& c) {
        const uint32_t self = self_form(c), other = form_of(c.args[0]);
        if (!other) {
            c.vm.log_error(c.stack, "Cannot get the heading angle to a None object");
            c.result = Value::of(0.0f);
            return;
        }
        const auto a = position(self), b = position(other);
        const float d = degrees(std::atan2(b[0] - a[0], b[1] - a[1]) - angle(self)[2]);
        c.result = Value::of(std::remainder(d, 360.0f));
    });
    vm.bind_native("ObjectReference", "GetNthLinkedRef", [&g](NativeCall& c) {
        // The last one reached when the chain ends early, not None.
        uint32_t ref = self_form(c);
        for (int32_t i = 0; i < c.args[0].integer; ++i) {
            const uint32_t next = g.conditions().linked_ref(ref, 0);
            if (!next) {
                break;
            }
            ref = next;
        }
        c.result = c.vm.form_value(ref, ref_class(g.db(), ref));
    });
    vm.bind_native("ObjectReference", "HasRefType", [&g](NativeCall& c) {
        const uint32_t type = form_of(c.args[0]);
        if (!type) {
            c.vm.log_error(c.stack, "Cannot check if the reference has a None location ref type");
            c.result = Value::of(false);
            return;
        }
        c.result = Value::of(g.conditions().value(561, self_form(c), 0, type, 0) != 0.0);
    });
    // 1 open, 3 closed; only activators, containers and doors have one. No sequences run here yet, so never 2 or 4.
    const auto open_state = [&g](uint32_t ref) {
        const forms::FormEntry* b = g.db().find(g.base_of(ref));
        if (!b || (b->type != forms::sig("ACTI") && b->type != forms::sig("CONT") && b->type != forms::sig("DOOR"))) {
            return 0;
        }
        const game::RefChanges* c = g.find_changes(ref);
        return c && c->open_state ? c->open_state : 3;
    };
    vm.bind_native("ObjectReference", "GetOpenState", [open_state](NativeCall& c) { c.result = Value::of(int32_t(open_state(self_form(c)))); });
    vm.bind_native("ObjectReference", "SetOpen", [&g, host, open_state](NativeCall& c) {
        const uint32_t self = self_form(c);
        const int state = open_state(self);
        const bool open = c.args[0].boolean;
        if (state && open != (state == 1 || state == 2)) {
            g.changes(self).open_state = open ? 1 : 3;
            host.activate(self, 0, false);
            host.script_door(self, open);
        }
    });
    vm.bind_native("ObjectReference", "GetTriggerObjectCount", [host](NativeCall& c) { c.result = Value::of(host.trigger_count(self_form(c))); });
    const auto bound = [&g](uint32_t ref, int axis) {
        const forms::FormEntry* e = g.db().find(g.base_of(ref));
        const forms::Value* o = e ? forms::Form(g.db(), *e).get("OBND") : nullptr;
        const char* const lo[] = {"x1", "y1", "z1"};
        const char* const hi[] = {"x2", "y2", "z2"};
        const forms::Value* a = o ? o->member(lo[axis]) : nullptr;
        const forms::Value* b = o ? o->member(hi[axis]) : nullptr;
        return a && b ? float(b->integer - a->integer) : 0.0f;
    };
    vm.bind_native("ObjectReference", "GetWidth", [bound](NativeCall& c) { c.result = Value::of(bound(self_form(c), 0)); });
    vm.bind_native("ObjectReference", "GetLength", [bound](NativeCall& c) { c.result = Value::of(bound(self_form(c), 1)); });
    vm.bind_native("ObjectReference", "GetHeight", [bound](NativeCall& c) { c.result = Value::of(bound(self_form(c), 2)); });
    vm.bind_native("ObjectReference", "TetherToHorse", [host](NativeCall& c) {
        const uint32_t horse = form_of(c.args[0]);
        if (horse) {
            host.tether(self_form(c), horse);
        } else {
            c.vm.log_error(c.stack, "Cannot Tether to nothing");
        }
    });
    latent("SetMotionType", [host](NativeCall& c) {
        const int32_t type = c.args[0].integer;
        if (type < 1 || type > 6 || type == 5) {
            c.vm.log_error(c.stack, "Attempting to set an invalid motion type");
        }
        host.hold_cart(self_form(c), type == 4);
        c.vm.return_from_latent(c.stack, {}); // the bodies themselves wait for dynamic references
    });

    // ---- moving references ----
    const auto translate = [host](NativeCall& c, size_t speed_at) {
        Translation t;
        t.ref = self_form(c);
        for (size_t i = 0; i < 3; ++i) {
            t.position[i] = c.args[i].real;
            t.angle[i] = c.args[3 + i].real;
        }
        t.speed = c.args[speed_at].real;
        t.max_rotation = c.args[speed_at + 1].real;
        host.translate(t);
    };
    vm.bind_native("ObjectReference", "TranslateTo", [translate](NativeCall& c) { translate(c, 6); });
    vm.bind_native("ObjectReference", "SplineTranslateTo", [translate](NativeCall& c) { translate(c, 7); });
    vm.bind_native("ObjectReference", "StopTranslation", [host](NativeCall& c) { host.stop_translation(self_form(c)); });
    latent("MoveToNode", [&g, host, position](NativeCall& c) {
        const uint32_t self = self_form(c), target = form_of(c.args[0]);
        if (!target) {
            c.vm.log_error(c.stack, std::format("{:08X} cannot be moved to a None object's location", self));
            c.vm.return_from_latent(c.stack, {});
            return;
        }
        auto p = position(target);
        const auto offset = host.node_offset(target, c.vm.text(c.args[1].string));
        for (size_t i = 0; i < 3; ++i) {
            p[i] += offset[i];
        }
        if (self == k_player_ref) {
            host.move_player(p);
        } else {
            g.move_ref(self, p);
            host.moved(self);
            host.move_agent(self, 0, p);
        }
        c.vm.return_from_latent(c.stack, {});
    });
    vm.bind_native("Cell", "SetPublic", [&g](NativeCall& c) {
        g.set_ref_flag(self_form(c), game::ref_public, c.args[0].boolean);
        g.set_ref_flag(self_form(c), game::ref_not_public, !c.args[0].boolean);
    });
    vm.bind_native("ObjectReference", "GetEditorLocation", [&g](NativeCall& c) {
        c.result = c.vm.form_value(g.conditions().location_of(self_form(c)), "Location");
    });

    // ---- ownership ----
    for (const char* cls : {"ObjectReference", "Cell"}) {
        vm.bind_native(cls, "SetFactionOwner", [&g](NativeCall& c) { g.changes(self_form(c)).owner = form_of(c.args[0]); });
        vm.bind_native(cls, "SetActorOwner", [&g](NativeCall& c) { g.changes(self_form(c)).owner = form_of(c.args[0]); });
        const auto owner = [&g](NativeCall& c, uint32_t type, const char* cls) {
            const uint32_t own = g.own_owner(self_form(c));
            const forms::FormEntry* e = g.db().find(own);
            c.result = c.vm.form_value(e && e->type == type ? own : 0, cls);
        };
        vm.bind_native(cls, "GetFactionOwner", [owner](NativeCall& c) { owner(c, forms::sig("FACT"), "Faction"); });
        vm.bind_native(cls, "GetActorOwner", [owner](NativeCall& c) { owner(c, forms::sig("NPC_"), "ActorBase"); });
    }
    vm.bind_native("ObjectReference", "SetNoFavorAllowed", [&g](NativeCall& c) { g.set_ref_flag(self_form(c), game::ref_no_favor, c.args[0].boolean); });
    vm.bind_native("ObjectReference", "IgnoreFriendlyHits", [&g](NativeCall& c) { g.set_ref_flag(self_form(c), game::ref_ignore_friendly_hits, c.args[0].boolean); });
    vm.bind_native("ObjectReference", "IsFurnitureInUse", [&g](NativeCall& c) { c.result = Value::of(g.ai().furniture_in_use(self_form(c)) || g.player_furniture() == self_form(c)); });
}

} // namespace spud::engine
