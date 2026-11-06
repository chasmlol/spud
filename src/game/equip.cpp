// Equipping and using what the player carries: ActorEquipManager's toggle and what each form does
// "Equipping".

#include "formats/forms_typed.hpp"
#include "game/game.hpp"

#include <algorithm>

namespace spud::game {

namespace {

// EQUP forms.
constexpr uint32_t k_slot_right = 0x00013F42, k_slot_left = 0x00013F43, k_slot_either = 0x00013F44,
                   k_slot_both = 0x00013F45, k_slot_shield = 0x000141E8;

// The default objects "Pickup Sound ..." and "Putdown Sound ...".
constexpr uint32_t k_pickup_generic = 0x0003C7BA, k_putdown_generic = 0x0003C7B9, k_pickup_weapon = 0x0003C7BE,
                   k_putdown_weapon = 0x0003C7C0, k_pickup_armor = 0x0003E609, k_putdown_armor = 0x0003E60B,
                   k_pickup_book = 0x0003EDDE, k_putdown_book = 0x0003EE0C, k_pickup_ingredient = 0x0003C7C2,
                   k_putdown_ingredient = 0x0003C7C3;

bool in_hand(const Item& item) {
    return item.kind == ItemKind::weapon || item.kind == ItemKind::scroll || item.kind == ItemKind::light;
}

} // namespace

std::string Game::item_sound(const Item& item, bool pickup, bool use) const {
    uint32_t form = 0;
    if (use) {
        form = item.consume_sound;
    } else {
        form = pickup ? item.pickup_sound : item.putdown_sound;
    }
    if (!form) {
        switch (item.kind) {
        case ItemKind::weapon: form = pickup ? k_pickup_weapon : k_putdown_weapon; break;
        case ItemKind::armor: form = pickup ? k_pickup_armor : k_putdown_armor; break;
        case ItemKind::book: form = pickup ? k_pickup_book : k_putdown_book; break;
        case ItemKind::ingredient: form = pickup ? k_pickup_ingredient : k_putdown_ingredient; break;
        default: form = pickup ? k_pickup_generic : k_putdown_generic; break;
        }
    }
    const forms::FormEntry* e = m_db->find(form);
    return e ? std::string(m_db->editor_id(*e)) : std::string();
}

void Game::unequip_where(const std::function<bool(const Item&, const Stack&)>& test) {
    for (Stack& s : m_player.inventory.stacks()) {
        const Item* item = s.worn == Worn::none ? nullptr : m_items.find(s.form);
        if (item && test(*item, s)) {
            s.worn = Worn::none;
            if (on_unequipped) {
                on_unequipped(item->form);
            }
        }
    }
}

// The hand an item goes to: a two-handed weapon takes both, the caller's hand wins for what fits either.
void Game::equip_hand(Stack& stack, const Item& item, int hand) {
    const bool both_hands = item.equip_slot == k_slot_both;
    const bool shield = item.equip_slot == k_slot_shield;
    bool left = false;
    if (item.equip_slot == k_slot_left || item.kind == ItemKind::light || shield) {
        left = true;
    } else if (item.kind == ItemKind::scroll) {
        left = hand != 0;
    } else if (!both_hands && (item.equip_slot == k_slot_either || item.equip_slot == k_slot_right)) {
        left = hand == 1;
    }
    const auto holds_left = [this](const Item& i, const Stack& s) {
        return s.worn == Worn::left || s.worn == Worn::both || i.equip_slot == k_slot_both ||
               i.equip_slot == k_slot_shield;
    };
    const auto holds_right = [](const Item& i, const Stack& s) {
        return s.worn == Worn::right || s.worn == Worn::both || i.equip_slot == k_slot_both;
    };
    const uint32_t form = stack.form;
    // Hand conflicts: a spell in the hand is on the process's equipped list like anything else.
    if (both_hands || left) {
        m_casters.select(k_player_ref, source_left, 0);
    }
    if (both_hands || !left) {
        m_casters.select(k_player_ref, source_right, 0);
    }
    if (both_hands) {
        unequip_where([&](const Item& i, const Stack& s) { return in_hand(i) || holds_left(i, s) || holds_right(i, s); });
    } else if (left) {
        unequip_where([&](const Item& i, const Stack& s) {
            return (in_hand(i) || i.equip_slot == k_slot_shield) && holds_left(i, s) && (i.form != form || s.worn != Worn::right);
        });
    } else {
        unequip_where([&](const Item& i, const Stack& s) {
            return in_hand(i) && holds_right(i, s) && (i.form != form || s.worn != Worn::left);
        });
    }
    if (shield) {
        stack.worn = Worn::worn;
    } else if (stack.worn == Worn::left || stack.worn == Worn::right) {
        stack.worn = Worn::both; // a second of the stack in the other hand
    } else {
        stack.worn = left ? Worn::left : Worn::right;
    }
}

// ActorEquipManager's spell: the hand's slot (0 right, 1 left, below 0 none) by's rule, and what
// overlaps it comes off. No slot: left, or right when only the left is taken.
void Game::equip_spell(uint32_t form, int hand) {
    const Spell* spell = m_magic.spell(form);
    const bool both = spell && spell->equip_slot == k_slot_both;
    const auto holds_left = [](const Item& i, const Stack& s) {
        return s.worn == Worn::left || s.worn == Worn::both || i.equip_slot == k_slot_both || i.equip_slot == k_slot_shield;
    };
    const auto holds_right = [](const Item& i, const Stack& s) {
        return s.worn == Worn::right || s.worn == Worn::both || i.equip_slot == k_slot_both;
    };
    const auto taken = [&](auto&& holds, uint8_t source) {
        bool any = m_casters.selected(k_player_ref, source) != 0;
        for (const Stack& s : m_player.inventory.stacks()) {
            const Item* item = s.worn == Worn::none ? nullptr : m_items.find(s.form);
            any = any || (item && (in_hand(*item) || item->equip_slot == k_slot_shield) && holds(*item, s));
        }
        return any;
    };
    if (hand < 0) {
        hand = taken(holds_left, source_left) && !taken(holds_right, source_right) ? 0 : 1;
    }
    if (both || hand == 1) {
        unequip_where([&](const Item& i, const Stack& s) { return (in_hand(i) || i.equip_slot == k_slot_shield) && holds_left(i, s); });
        m_casters.select(k_player_ref, source_left, form);
    }
    if (both || hand == 0) {
        unequip_where([&](const Item& i, const Stack& s) { return in_hand(i) && holds_right(i, s); });
        m_casters.select(k_player_ref, source_right, form);
    }
}

void Game::notify(std::string text, std::string sound) { m_notices.push_back({std::move(text), std::move(sound)}); }

std::vector<Notice> Game::take_notices() { return std::exchange(m_notices, {}); }

void Game::eat_ingredient(const Item& item) {
    // Perk entry point 72 gives how many effects become known, 1 without perks: eating teaches the first.
    const auto n = size_t(std::clamp(perk_value(72, 1.0f) + 0.5f, 0.0f, float(item.effects.size())));
    uint16_t& known = m_player.ingredient_effects[item.form];
    for (size_t i = 0; i < n; ++i) {
        if (known & (1u << i)) {
            continue;
        }
        known = uint16_t(known | (1u << i));
        const MagicEffect* m = m_magic.effect(item.effects[i].effect);
        std::string line = m_settings.text("sDiscoveredIngredientEffectEating");
        for (const std::string* text : {m ? &m->name : nullptr, &item.name}) {
            if (const size_t at = line.find("%s"); text && at != std::string::npos) {
                line.replace(at, 2, *text);
            }
        }
        notify(std::move(line), "UIAlchemyLearnEffect");
    }
}

ItemUse Game::poison_weapon(size_t index) {
    // (the game asks first; spud applies straight away).
    const uint32_t poison = m_player.inventory.stacks()[index].form;
    Stack* weapon = nullptr;
    const Item* item = nullptr;
    for (Stack& s : m_player.inventory.stacks()) {
        const Item* i = s.worn == Worn::right || s.worn == Worn::both || s.worn == Worn::worn ? m_items.find(s.form) : nullptr;
        if (i && i->kind == ItemKind::weapon) {
            weapon = &s;
            item = i;
        }
    }
    if (!item || item->animation_type == 0) {
        notify(m_settings.text("sPoisonNoWeaponMessage"), "UIMenuCancel");
        return ItemUse::nothing;
    }
    if (item->animation_type == 8) {
        notify(m_settings.text("sPoisonUnableToPoison"), "UIMenuCancel");
        return ItemUse::nothing;
    }
    if (weapon->extra.poison) {
        notify(m_settings.text("sPoisonAlreadyPoisonedMessage"), "UIMenuCancel");
        return ItemUse::nothing;
    }
    weapon->extra.poison = poison;
    weapon->extra.poison_doses = int32_t(std::max(std::floor(perk_value(83, 1.0f, {item->form, poison})), 1.0f));
    m_player.inventory.remove_at(index, 1);
    sound("ITMPoisonUse");
    return ItemUse::changed;
}

ItemUse Game::select(size_t index, int hand) {
    Stack& stack = m_player.inventory.stacks().at(index);
    const Item* item = m_items.find(stack.form);
    if (!item) {
        return ItemUse::nothing;
    }
    switch (item->kind) {
    case ItemKind::armor:
        if (stack.worn != Worn::none) {
            stack.worn = Worn::none;
            sound(item_sound(*item, false, false));
            return ItemUse::changed;
        }
        // Everything in the slots the piece covers comes off.
        unequip_where([&](const Item& i, const Stack&) {
            return i.kind == ItemKind::armor && (i.biped_slots & item->biped_slots);
        });
        if (item->equip_slot == k_slot_shield) {
            equip_hand(stack, *item, hand);
        } else {
            stack.worn = Worn::worn;
        }
        sound(item_sound(*item, true, false));
        return ItemUse::changed;
    case ItemKind::ammo:
        if (stack.worn != Worn::none) {
            stack.worn = Worn::none;
            sound(item_sound(*item, false, false));
            return ItemUse::changed;
        }
        unequip_where([](const Item& i, const Stack&) { return i.kind == ItemKind::ammo; });
        stack.worn = Worn::worn;
        sound(item_sound(*item, true, false));
        return ItemUse::changed;
    case ItemKind::weapon:
    case ItemKind::scroll:
    case ItemKind::light: {
        const bool chosen = hand >= 0;
        const Worn here = hand == 1 ? Worn::left : Worn::right;
        if (stack.worn == Worn::both) {
            stack.worn = !chosen ? Worn::none : here == Worn::left ? Worn::right : Worn::left;
            sound(item_sound(*item, false, false));
            return ItemUse::changed;
        }
        const bool either = item->equip_slot == k_slot_either || item->equip_slot == k_slot_right ||
                            item->equip_slot == k_slot_left;
        if (stack.worn != Worn::none) {
            if (chosen && stack.worn != here && either && item->kind != ItemKind::light) {
                if (stack.count > 1) {
                    equip_hand(stack, *item, hand); // a second copy in the other hand
                } else {
                    stack.worn = Worn::none;
                    equip_hand(stack, *item, hand); // moved
                }
                sound(item_sound(*item, true, false));
            } else {
                stack.worn = Worn::none;
                sound(item_sound(*item, false, false));
            }
            return ItemUse::changed;
        }
        equip_hand(stack, *item, hand);
        if (item->kind == ItemKind::light) {
            sound("ITMTorchHeldEquip");
        } else {
            sound(item_sound(*item, true, false));
        }
        if (item->kind == ItemKind::weapon) {
            ensure_ammo(*item);
        }
        if (on_equipped) {
            on_equipped(item->form);
        }
        return ItemUse::changed;
    }
    case ItemKind::ingredient: {
        const Item& ingredient = *item;
        eat_ingredient(ingredient);
        m_character->apply(ingredient.form, k_player_ref, m_player.ingredient_effects[ingredient.form]);
        sound(item_sound(ingredient, true, true));
        m_player.inventory.remove_at(index, 1);
        return ItemUse::changed;
    }
    case ItemKind::potion:
        if (item->poison()) {
            return poison_weapon(index);
        }
        m_character->apply(item->form);
        sound(item_sound(*item, true, true));
        m_player.inventory.remove_at(index, 1);
        return ItemUse::changed;
    case ItemKind::book:
        sound(item_sound(*item, true, false));
        return ItemUse::read;
    default:
        notify(m_settings.text("sCantEquipGeneric"), "UIMenuCancel");
        return ItemUse::nothing;
    }
}

void Game::ensure_ammo(const Item& weapon) {
    // the wrong kind of ammo gives way to the lowest-damage right one.
    if (weapon.animation_type != 7 && weapon.animation_type != 9) {
        return;
    }
    const bool bolts = weapon.animation_type == 9;
    const auto fits = [bolts](const Item& a) { return ((a.weapon_flags & 4) == 0) == bolts; }; // flag 4: non-bolt
    Stack* best = nullptr;
    int lowest = 0;
    for (Stack& s : m_player.inventory.stacks()) {
        const Item* a = m_items.find(s.form);
        if (!a || a->kind != ItemKind::ammo) {
            continue;
        }
        if (s.worn != Worn::none && fits(*a)) {
            return;
        }
        if (fits(*a) && !(a->weapon_flags & 2) && (!best || a->damage < lowest)) {
            best = &s;
            lowest = a->damage;
        }
    }
    if (best) {
        unequip_where([](const Item& i, const Stack&) { return i.kind == ItemKind::ammo; });
        best->worn = Worn::worn;
    }
}

bool Game::equip_form(uint32_t form, bool on) {
    auto& stacks = m_player.inventory.stacks();
    for (size_t i = 0; i < stacks.size(); ++i) {
        if (stacks[i].form == form && (stacks[i].worn != Worn::none) != on) {
            select(i, -1);
            return true;
        }
    }
    return false;
}

void Game::unequip_all() {
    for (Stack& s : m_player.inventory.stacks()) {
        s.worn = Worn::none;
    }
}

bool Game::is_equipped(uint32_t form) const {
    const auto& stacks = m_player.inventory.stacks();
    return std::any_of(stacks.begin(), stacks.end(), [form](const Stack& s) { return s.form == form && s.worn != Worn::none; });
}

uint32_t Game::equipped_weapon(bool left) const {
    for (const Stack& s : m_player.inventory.stacks()) {
        const Item* item = m_items.find(s.form);
        if (item && item->kind == ItemKind::weapon &&
            (s.worn == Worn::both || s.worn == (left ? Worn::left : Worn::right))) {
            return s.form;
        }
    }
    return 0;
}

int Game::equipped_item_type(bool left) const {
    // hold what a hand has, spells too: 9.
    if (m_casters.selected(k_player_ref, left ? source_left : source_right)) {
        return 9;
    }
    // A weapon's animation type, the crossbow's 9 reading 12.
    if (const Item* item = m_items.find(equipped_weapon(left))) {
        return item->animation_type == 9 ? 12 : item->animation_type;
    }
    for (const Stack& s : m_player.inventory.stacks()) {
        const Item* item = m_items.find(s.form);
        if (item && s.worn != Worn::none && left && item->kind == ItemKind::light) {
            return 11;
        }
        if (item && s.worn != Worn::none && left && item->equip_slot == k_slot_shield) {
            return 10;
        }
    }
    return 0;
}

bool Game::read_book(const Item& book, uint32_t ref) {
    const bool first = m_player.books_read.insert(book.form).second;
    if (on_read) {
        on_read(book.form, ref);
    }
    if (book.book_flags & book_teaches_spell) {
        const forms::FormEntry* e = m_db->find(book.book_spell);
        const std::string name = e ? std::string(forms::Form(*m_db, *e).name()) : std::string();
        if (!m_character->add_spell(book.book_spell)) {
            notify(std::format("{} {}", m_settings.text("sAlreadyKnown"), name));
            return false;
        }
        notify(std::format("{}: {}", m_settings.text("sSpellAdded"), name), "UISpellLearned");
        return true;
    }
    if (first && (book.book_flags & book_teaches_skill) && book.book_skill >= k_first_skill &&
        book.book_skill < k_first_skill + k_skill_count && m_character->values().permanent(book.book_skill) < 200.0f) {
        increment_skill(book.book_skill, int(perk_value(5, 1.0f)));
    }
    return false;
}

} // namespace spud::game
