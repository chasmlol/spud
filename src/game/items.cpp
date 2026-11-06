#include "game/items.hpp"

#include "core/path.hpp"

#include <algorithm>

namespace spud::game {

namespace {

using forms::Form;
using forms::Value;

constexpr forms::Sig sig(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
           uint32_t(uint8_t(s[3])) << 24;
}

int64_t integer(const Form& f, std::string_view path, int64_t fallback = 0) {
    const Value* v = f.get(path);
    if (!v) {
        return fallback;
    }
    return v->kind == Value::Kind::integer ? v->integer
           : v->kind == Value::Kind::real  ? int64_t(v->real)
           : v->kind == Value::Kind::form  ? int64_t(v->form)
                                           : fallback;
}

float real(const Form& f, std::string_view path, float fallback = 0) {
    const Value* v = f.get(path);
    if (!v) {
        return fallback;
    }
    return v->kind == Value::Kind::real      ? float(v->real)
           : v->kind == Value::Kind::integer ? float(v->integer)
                                             : fallback;
}

uint32_t form_of(const Form& f, std::string_view path) {
    const Value* v = f.get(path);
    return v && v->kind == Value::Kind::form ? v->form : 0;
}

std::string text(const Form& f, std::string_view path) {
    const Value* v = f.get(path);
    return v && v->kind == Value::Kind::text ? v->text : std::string();
}

ItemKind kind_of(forms::Sig type) {
    switch (type) {
    case sig("ARMO"): return ItemKind::armor;
    case sig("WEAP"): return ItemKind::weapon;
    case sig("AMMO"): return ItemKind::ammo;
    case sig("ALCH"): return ItemKind::potion;
    case sig("SCRL"): return ItemKind::scroll;
    case sig("INGR"): return ItemKind::ingredient;
    case sig("BOOK"): return ItemKind::book;
    case sig("KEYM"): return ItemKind::key;
    case sig("MISC"): return ItemKind::misc;
    case sig("SLGM"): return ItemKind::soul_gem;
    case sig("APPA"): return ItemKind::apparatus;
    case sig("LIGH"): return ItemKind::light;
    default: return ItemKind::none;
    }
}

// The model of a static, for the items whose menus show a separate form.
std::string static_model(const forms::Database& db, uint32_t form) {
    const forms::FormEntry* e = form ? db.find(form) : nullptr;
    return e ? model_path(text(Form(db, *e), "MODL")) : std::string();
}

void read_item(const forms::Database& db, const Form& f, Item& i) {
    i.editor_id = f.editor_id();
    i.name = f.name();
    i.value = int32_t(forms::item_value(f).value_or(0));
    // Outside survival mode ammo, gold and lockpicks weigh nothing.
    const bool weightless = i.kind == ItemKind::ammo || i.form == k_gold || i.form == k_lockpick;
    i.weight = weightless ? 0.0f : std::max(float(forms::item_weight(f).value_or(0)), 0.0f);
    if (weightless && i.form != k_gold) {
        i.survival_weight = std::max(float(forms::item_weight(f).value_or(0)), 0.0f);
    }
    i.keywords = f.keywords();
    i.pickup_sound = form_of(f, "YNAM");
    i.putdown_sound = form_of(f, "ZNAM");
    i.equip_slot = form_of(f, "ETYP");
    i.enchantment = form_of(f, "EITM");
    i.enchantment_amount = uint16_t(integer(f, "EAMT"));
    i.model = model_path(text(f, "MODL"));
    i.inventory_model = i.model;
    i.description = text(f, "DESC");
    switch (i.kind) {
    case ItemKind::armor: {
        i.model = model_path(text(f, "MOD2"));
        i.inventory_model = i.model;
        const bool bod2 = f.get("BOD2") != nullptr;
        i.biped_slots = uint32_t(integer(f, bod2 ? "BOD2.biped_flags" : "BODT.biped_flags"));
        i.armor_type = uint32_t(integer(f, bod2 ? "BOD2.armor_type" : "BODT.armor_type"));
        i.armor_rating = int32_t(integer(f, "DNAM"));
        i.template_form = form_of(f, "TNAM");
        i.armor_flags = uint32_t(integer(f, "BODT.general_flags"));
        break;
    }
    case ItemKind::weapon:
        i.damage = int32_t(integer(f, "game_data.damage"));
        // Read, and by the swing and draw handlers.
        i.swing_sound = form_of(f, "TNAM");
        i.draw_sound = form_of(f, "NAM9");
        i.sheathe_sound = form_of(f, "NAM8");
        i.animation_type = uint8_t(integer(f, "DNAM.animation_type"));
        i.weapon_flags = uint32_t(integer(f, "DNAM.flags"));
        i.skill = int32_t(integer(f, "DNAM.skill", -1));
        i.speed = real(f, "DNAM.speed", 1);
        i.reach = real(f, "DNAM.reach", 1);
        i.resist = int32_t(integer(f, "DNAM.resist", -1));
        i.stagger = real(f, "DNAM.stagger", 0);
        i.crit_damage = int32_t(integer(f, "CRDT.damage"));
        i.crit_mult = real(f, "CRDT.percent_mult", 1);
        i.crit_effect = form_of(f, "CRDT.effect");
        i.crit_on_death = integer(f, "CRDT.on_death") != 0;
        i.template_form = form_of(f, "CNAM");
        i.projectile_count = uint32_t(integer(f, "DNAM.projectile_count", 1));
        i.attack_sound = form_of(f, "SNAM");
        i.attack_sound_2d = form_of(f, "XNAM");
        i.impact_set = form_of(f, "INAM");
        i.bash_impact_set = form_of(f, "BIDS");
        i.sound_level = int32_t(integer(f, "VNAM"));
        if (std::string first = static_model(db, form_of(f, "WNAM")); !first.empty()) {
            i.inventory_model = std::move(first);
        }
        break;
    case ItemKind::ammo:
        i.damage = int32_t(real(f, "DATA.damage"));
        i.weapon_flags = uint32_t(integer(f, "DATA.flags"));
        i.projectile = form_of(f, "DATA.projectile");
        break;
    case ItemKind::book:
        i.book_flags = uint8_t(integer(f, "DATA.flags"));
        i.book_type = uint8_t(integer(f, "DATA.type"));
        i.book_text = i.description;
        i.description = text(f, "CNAM");
        if (i.book_flags & book_teaches_skill) {
            i.book_skill = int32_t(integer(f, "DATA.teaches.skill", -1));
        } else if (i.book_flags & book_teaches_spell) {
            i.book_spell = form_of(f, "DATA.teaches.spell");
        }
        if (std::string art = static_model(db, form_of(f, "INAM")); !art.empty()) {
            i.inventory_model = std::move(art);
        }
        break;
    case ItemKind::soul_gem:
        i.soul = uint8_t(integer(f, "SOUL"));
        i.soul_capacity = uint8_t(integer(f, "SLCP"));
        i.soul_gem_filled = form_of(f, "NAM0");
        break;
    case ItemKind::potion:
    case ItemKind::ingredient:
    case ItemKind::scroll:
        i.effect_flags = uint32_t(integer(f, "ENIT.flags"));
        i.consume_sound = form_of(f, "ENIT.consume_sound");
        for (const forms::EffectEntry& fx : forms::effects(f)) {
            i.effects.push_back({fx.effect, fx.magnitude, fx.area, fx.duration});
        }
        break;
    default:
        break;
    }
}

} // namespace

bool Item::has_keyword(uint32_t keyword) const {
    return std::find(keywords.begin(), keywords.end(), keyword) != keywords.end();
}

std::string model_path(std::string_view modl) {
    if (modl.empty()) {
        return {};
    }
    return path::replace_extension(path::with_root(path::normalize(modl), "meshes"), ".osm");
}

uint32_t Items::create(Item item) const {
    const uint32_t form = m_next_created++;
    item.form = form;
    m_items[form] = std::make_unique<Item>(std::move(item));
    return form;
}

const Item* Items::find(uint32_t form) const {
    if (const auto it = m_items.find(form); it != m_items.end()) {
        return it->second.get();
    }
    std::unique_ptr<Item> out;
    const forms::FormEntry* e = m_db->find(form);
    const ItemKind kind = e && !(e->flags & forms::form_deleted) ? kind_of(e->type) : ItemKind::none;
    if (kind != ItemKind::none) {
        const Form f(*m_db, *e);
        // A light is an item only when it can be carried (LIGH DATA flag 0x2).
        if (kind != ItemKind::light || (integer(f, "DATA.flags") & 0x2)) {
            out = std::make_unique<Item>();
            out->form = form;
            out->kind = kind;
            read_item(*m_db, f, *out);
        }
    }
    return m_items.emplace(form, std::move(out)).first->second.get();
}

} // namespace spud::game
