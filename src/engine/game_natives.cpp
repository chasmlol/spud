// Papyrus natives that act on the game's item and crime state (src/game): bound over the VM's stand-ins once both
// exist.

#include "engine/game_natives.hpp"

#include "game/game.hpp"
#include "script/natives.hpp"
#include "script/vm.hpp"

#include <algorithm>
#include <format>

namespace spud::engine {

namespace {

using script::NativeCall;
using script::Tag;
using script::Value;

constexpr uint32_t k_player_ref = 0x14;

uint32_t form_of(const Value& v) {
    return v.tag == Tag::object && v.object ? script::handle_form(v.object->handle) : 0;
}

uint32_t self_form(const NativeCall& c) { return c.self ? script::handle_form(c.self->handle) : 0; }

game::Inventory& inventory_of(game::Game& g, uint32_t ref) {
    return ref == k_player_ref ? g.player().inventory : g.container(ref);
}

// "Iron Sword Added", "Gold (5) Removed": the player's notification.
void tell(game::Game& g, uint32_t form, int32_t count, bool added) {
    const game::Item* item = g.items().find(form);
    if (!item) {
        return;
    }
    const std::string& word = g.settings().text(added ? "sAddItemtoInventory" : "sRemoveItemfromInventory");
    g.notify(count > 1 ? std::format("{} ({}) {}", item->name, count, word) : std::format("{} {}", item->name, word));
}

} // namespace

void bind_game_natives(script::VirtualMachine& vm, game::Game& g) {
    // Mounts. Only the player rides.
    vm.bind_native("Actor", "IsOnMount", [&g](NativeCall& c) {
        c.result = Value::of(self_form(c) == game::k_player_ref && g.ridden_horse != 0);
    });
    vm.bind_native("Actor", "IsBeingRidden", [&g](NativeCall& c) {
        c.result = Value::of(g.ridden_horse != 0 && self_form(c) == g.ridden_horse);
    });
    vm.bind_native("Actor", "Dismount", [&g](NativeCall& c) {
        const bool mine = self_form(c) == game::k_player_ref && g.ridden_horse != 0;
        g.dismount_asked = mine;
        c.result = Value::of(mine);
    });
    vm.bind_native("Game", "GetPlayersLastRiddenHorse", [&g](NativeCall& c) {
        c.result = g.last_horse() ? c.vm.form_value(g.last_horse(), "Actor") : Value{};
    });
    vm.bind_native("Actor", "IsFlying", [&g](NativeCall& c) {
        const game::Agent* a = g.ai().agent(self_form(c));
        c.result = Value::of(a && a->fly != game::FlyState::none && a->fly != game::FlyState::perching);
    });
    vm.bind_native("Actor", "GetFlyingState", [&g](NativeCall& c) {
        const game::Agent* a = g.ai().agent(self_form(c));
        c.result = Value::of(a ? int32_t(a->fly) : 0);
    });
    vm.bind_native("Actor", "SetAllowFlying", [&g](NativeCall& c) {
        if (game::Agent* a = g.ai().mutable_agent(self_form(c))) {
            a->allow_flying = c.args.empty() || c.args[0].boolean;
            a->flight.line.clear(); // it looks for a landing marker on the next step
        }
    });
    // The quest a package belongs to, its record's QNAM: what its own fragments set stages on.
    vm.bind_native("Package", "GetOwningQuest", [&g](NativeCall& c) {
        const forms::FormEntry* e = g.db().find(self_form(c));
        const forms::Value* q = e ? forms::Form(g.db(), *e).get("QNAM") : nullptr;
        c.result = c.vm.form_value(q && q->kind == forms::Value::Kind::form ? q->form : 0, "Quest");
    });
    const auto forms_of = [&g](uint32_t form) {
        // A list stands for each of its forms.
        if (const uint32_t base = g.placed_base(form)) {
            return std::vector<uint32_t>{base}; // a reference made at run time goes in as its item
        }
        const forms::FormEntry* e = g.db().find(form);
        return e && e->type == forms::sig("FLST") ? forms::form_list(forms::Form(g.db(), *e)) : std::vector<uint32_t>{form};
    };
    vm.bind_native("ObjectReference", "GetItemCount", [&g](NativeCall& c) {
        c.result = Value::of(g.count_of(inventory_of(g, self_form(c)), form_of(c.args[0])));
    });
    vm.bind_native("ObjectReference", "AddItem", [&g, forms_of](NativeCall& c) {
        const uint32_t self = self_form(c);
        const int32_t count = c.args[1].integer;
        for (const uint32_t form : forms_of(form_of(c.args[0]))) {
            // A levelled list is rolled at the container's level: its encounter zone's, else the player's (spud doesn't
            // keep zones yet, so the player's). A quest reward's gold comes this way.
            std::vector<game::Rolled> rolled;
            g.leveled().roll(form, count, uint16_t(std::max(g.character().level(), 1)), rolled);
            for (const game::Rolled& r : rolled) {
                inventory_of(g, self).add(r.form, r.count, r.extra);
                if (self == k_player_ref && !c.args[2].boolean) {
                    tell(g, r.form, r.count, true);
                }
            }
        }
        c.vm.return_from_latent(c.stack, {});
    }, script::native_flags("ObjectReference", "AddItem"));
    vm.bind_native("ObjectReference", "RemoveItem", [&g, forms_of](NativeCall& c) {
        const uint32_t self = self_form(c);
        for (const uint32_t form : forms_of(form_of(c.args[0]))) {
            const int32_t removed = inventory_of(g, self).remove(form, c.args[1].integer);
            if (const uint32_t other = form_of(c.args[3])) {
                inventory_of(g, other).add(form, removed);
            }
            if (self == k_player_ref && !c.args[2].boolean && removed) {
                tell(g, form, removed, false);
            }
        }
        c.vm.return_from_latent(c.stack, {});
    }, script::native_flags("ObjectReference", "RemoveItem"));
}

} // namespace spud::engine
