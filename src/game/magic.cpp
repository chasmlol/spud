#include "game/magic.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace spud::game {

namespace {

using forms::Form;
using forms::Value;

int64_t integer(const Form& f, std::string_view path, int64_t fallback = 0) {
    const Value* v = f.get(path);
    return v && v->kind == Value::Kind::integer ? v->integer : fallback;
}

float real(const Form& f, std::string_view path) {
    const Value* v = f.get(path);
    return v && v->kind == Value::Kind::real ? float(v->real) : 0.0f;
}

// Flags and forced actor values of table.
constexpr std::array<ArchetypeInfo, k_archetype_count> k_archetypes = {{
    {0x9A, -1}, {0x04, -1}, {0, -1}, {0, -1}, {0x02, -1}, {0x9A, -1}, {0x02, 0}, {0x02, 1}, {0x02, 0}, {0, -1}, {0, -1},
    {0x22, 54}, {0x104, -1}, {0x01, -1}, {0x03, 55}, {0, -1}, {0, -1}, {0x94, -1}, {0x94, -1}, {0x80, -1}, {0x80, -1},
    {0x12, 53}, {0x280, -1}, {0, -1}, {0x02, 1}, {0x04, -1}, {0, -1}, {0, -1}, {0, -1}, {0, -1}, {0, -1}, {0x12, -1},
    {0x9A, -1}, {0, -1}, {0x8E, -1}, {0x04, -1}, {0x04, -1}, {0x04, -1}, {0x02, 1}, {0x06, 85}, {0x04, -1}, {0, -1},
    {0x02, 1}, {0x44, -1}, {0, -1}, {0x12, 160}, {0x04, -1},
}};

} // namespace

const ArchetypeInfo& archetype_info(uint32_t archetype) {
    return k_archetypes[archetype < k_archetype_count ? archetype : 0];
}

const MagicEffect* Magic::effect(uint32_t form) const {
    if (const auto it = m_effects.find(form); it != m_effects.end()) {
        return it->second.get();
    }
    std::unique_ptr<MagicEffect> out;
    if (const forms::FormEntry* e = m_db->find(form)) {
        const Form f(*m_db, *e);
        out = std::make_unique<MagicEffect>();
        out->form = form;
        out->name = f.name();
        if (const Value* d = f.get("DNAM"); d && d->kind == Value::Kind::text) {
            out->description = d->text;
        }
        out->flags = uint32_t(integer(f, "DATA.flags"));
        out->base_cost = real(f, "DATA.base_cost");
        out->casting_type = uint32_t(integer(f, "DATA.casting_type"));
        out->delivery = uint32_t(integer(f, "DATA.delivery"));
        out->cast_time = real(f, "DATA.spellmaking_casting_time");
        out->magic_skill = int32_t(integer(f, "DATA.magic_skill", -1));
        out->minimum_skill = float(integer(f, "DATA.minimum_skill_level"));
        out->script_ai_score = real(f, "DATA.script_effect_ai_score");
        out->script_ai_delay = real(f, "DATA.script_effect_ai_delay_time");
        out->resist_value = int32_t(integer(f, "DATA.resist_value", -1));
        out->actor_value = int32_t(integer(f, "DATA.actor_value", -1));
        out->keywords = f.keywords();
        out->archetype = uint32_t(integer(f, "DATA.archetype"));
        if (const int forced = archetype_info(out->archetype).actor_value; forced >= 0) {
            out->actor_value = forced;
        }
        out->second_actor_value = int32_t(integer(f, "DATA.second_actor_value", -1));
        out->second_weight = real(f, "DATA.second_av_weight");
        out->taper_weight = real(f, "DATA.taper_weight");
        out->taper_curve = real(f, "DATA.taper_curve");
        out->taper_duration = real(f, "DATA.taper_duration");
        if (const Value* v = f.get("DATA.associated_item")) {
            out->associated = v->form;
        }
        if (const Value* list = f.get("SNDD")) {
            for (const Value& s : list->items) {
                const Value* type = s.member("type");
                const Value* sound = s.member("sound");
                if (type && sound && type->integer >= 0 && size_t(type->integer) < out->sounds.size()) {
                    out->sounds[size_t(type->integer)] = sound->form;
                }
            }
        }
        if (const Value* v = f.get("DATA.perk_to_apply")) {
            out->perk = v->form;
        }
        out->conditions = condition_list(f);
        out->has_scripts = f.get("VMAD") != nullptr;
        out->skill_mult = real(f, "DATA.skill_usage_multiplier");
        out->dual_scale = real(f, "DATA.dual_casting_scale");
        out->sound_level = uint32_t(integer(f, "DATA.casting_sound_level"));
        const auto linked = [&f](std::string_view path) {
            const Value* v = f.get(path);
            return v ? v->form : 0;
        };
        out->projectile = linked("DATA.projectile");
        out->explosion = linked("DATA.explosion");
        out->image_space = linked("DATA.image_space_modifier");
        out->casting_art = linked("DATA.casting_art");
        out->casting_light = linked("DATA.casting_light");
        out->hit_art = linked("DATA.hit_effect_art");
        out->hit_shader = linked("DATA.hit_shader");
        out->enchant_art = linked("DATA.enchant_art");
        out->enchant_shader = linked("DATA.enchant_shader");
        const auto visuals = [&](std::string_view path, uint32_t& art, uint32_t& shader) {
            if (const forms::FormEntry* rfct = m_db->find(linked(path))) {
                const Form r(*m_db, *rfct);
                const Value* a = r.get("DATA.effect_art");
                const Value* s = r.get("DATA.shader");
                art = a ? a->form : 0;
                shader = s ? s->form : 0;
            }
        };
        visuals("DATA.hit_visuals", out->hit_art, out->hit_shader);
        visuals("DATA.enchant_visuals", out->enchant_art, out->enchant_shader);
        if (const forms::FormEntry* dual = m_db->find(linked("DATA.dual_casting_art"))) {
            const Form d(*m_db, *dual);
            const Value* v = d.get("DATA.projectile");
            out->dual_projectile = v ? v->form : 0;
            const Value* art = d.get("DATA.hit_effect_art");
            const Value* shader = d.get("DATA.effect_shader");
            out->dual_art = art ? art->form : 0;
            out->dual_shader = shader ? shader->form : 0;
            out->dual_art_scales = (integer(d, "DATA.inherit_scale") & 1) != 0;
        }
    }
    return m_effects.emplace(form, std::move(out)).first->second.get();
}

const Spell* Magic::spell(uint32_t form) const {
    if (const auto it = m_spells.find(form); it != m_spells.end()) {
        return it->second.get();
    }
    std::unique_ptr<Spell> out;
    if (const auto c = m_created.find(form); c != m_created.end()) {
        out = std::make_unique<Spell>();
        out->form = form;
        out->type = spell_enchantment;
        out->cast_type = c->second.cast_type;
        out->manual_cost = true;
        out->base_cost = float(c->second.cost);
        for (const Effect& e : c->second.effects) {
            out->effects.push_back({e.effect, e.magnitude, e.area, e.duration, {}});
        }
    } else if (const forms::FormEntry* e = m_db->find(form)) {
        const Form f(*m_db, *e);
        out = std::make_unique<Spell>();
        out->form = form;
        out->name = f.name();
        out->keywords = f.keywords();
        const auto spell_data = [&] {
            out->flags = uint32_t(integer(f, "SPIT.flags"));
            out->cast_type = uint32_t(integer(f, "SPIT.cast_type"));
            out->delivery = uint32_t(integer(f, "SPIT.delivery"));
            out->manual_cost = (out->flags & 1) != 0;
            out->base_cost = float(integer(f, "SPIT.base_cost"));
            if (const Value* slot = f.get("ETYP")) {
                out->equip_slot = slot->form;
            }
            out->charge_time = real(f, "SPIT.charge_time");
            out->cast_duration = real(f, "SPIT.cast_duration");
            out->range = real(f, "SPIT.range");
            if (const Value* v = f.get("SPIT.half_cost_perk")) {
                out->half_cost_perk = v->form;
            }
        };
        switch (f.type()) {
        case forms::sig("SPEL"):
            out->type = uint32_t(integer(f, "SPIT.type"));
            spell_data();
            break;
        case forms::sig("SCRL"):
            out->type = spell_scroll;
            spell_data();
            break;
        case forms::sig("ENCH"): {
            const auto flags = integer(f, "ENIT.flags");
            out->type = integer(f, "ENIT.enchant_type") == spell_staff_enchantment ? spell_staff_enchantment : spell_enchantment;
            out->cast_type = uint32_t(integer(f, "ENIT.cast_type"));
            out->delivery = uint32_t(integer(f, "ENIT.delivery"));
            out->manual_cost = (flags & 1) != 0;
            out->extend_on_recast = (flags & 4) != 0;
            out->base_cost = float(integer(f, "ENIT.enchantment_cost"));
            out->charge_amount = int32_t(integer(f, "ENIT.enchantment_amount"));
            out->charge_time = real(f, "ENIT.charge_time");
            break;
        }
        case forms::sig("ALCH"):
            out->type = integer(f, "ENIT.flags") & 0x20000 ? spell_poison : spell_potion;
            break;
        case forms::sig("INGR"):
            out->type = spell_ingredient;
            out->food = (integer(f, "ENIT.flags") & 0x2) != 0;
            break;
        default: break;
        }
        SpellEffect* effect = nullptr;
        for (const forms::DecodedField& field : f.fields()) {
            if (field.sig == forms::sig("EFID")) {
                effect = &out->effects.emplace_back();
                effect->effect = field.value.form;
            } else if (effect && field.sig == forms::sig("EFIT")) {
                if (const Value* v = field.value.member("magnitude")) {
                    effect->magnitude = float(v->real);
                }
                if (const Value* v = field.value.member("area")) {
                    effect->area = uint32_t(v->integer);
                }
                if (const Value* v = field.value.member("duration")) {
                    effect->duration = uint32_t(v->integer);
                }
            } else if (effect && field.sig == forms::sig("CTDA")) {
                effect->conditions.push_back(condition_item(field.value));
            } else if (effect && !effect->conditions.empty() && (field.sig == forms::sig("CIS1") || field.sig == forms::sig("CIS2"))) {
                condition_text(effect->conditions.back(), field.value.text);
            }
        }
    }
    if (out) {
        for (const SpellEffect& e : out->effects) {
            const MagicEffect* m = effect(e.effect);
            out->hostile_count += m && (m->flags & effect_hostile) ? 1 : 0;
        }
    }
    return m_spells.emplace(form, std::move(out)).first->second.get();
}

const Shout* Magic::shout(uint32_t form) const {
    if (const auto it = m_shouts.find(form); it != m_shouts.end()) {
        return it->second.get();
    }
    std::unique_ptr<Shout> out;
    if (const forms::FormEntry* e = m_db->find(form); e && e->type == forms::sig("SHOU")) {
        const Form f(*m_db, *e);
        out = std::make_unique<Shout>();
        out->form = form;
        out->spells_as_powers = (e->flags & 0x80) != 0;
        size_t i = 0;
        for (const Value* w : f.all("SNAM")) {
            if (i < out->words.size()) {
                const Value* word = w->member("word");
                const Value* spell = w->member("spell");
                const Value* recovery = w->member("recovery_time");
                out->words[i++] = {word ? word->form : 0, spell ? spell->form : 0, recovery ? float(recovery->real) : 0.0f};
            }
        }
    }
    return m_shouts.emplace(form, std::move(out)).first->second.get();
}

const SpellEffect* Magic::costliest(const Spell& spell, bool with_projectile, bool with_skill) const {
    const SpellEffect* best = nullptr;
    float top = -1;
    for (const SpellEffect& e : spell.effects) {
        const MagicEffect* m = effect(e.effect);
        if (!m || (with_projectile && !m->projectile) || (with_skill && (m->magic_skill < 6 || m->magic_skill > 23))) {
            continue;
        }
        if (const float cost = effect_cost(e); cost > top) {
            top = cost;
            best = &e;
        }
    }
    return best;
}

bool Magic::same_effect(const SpellEffect& a, const SpellEffect& b) const {
    if (&a == &b) {
        return true;
    }
    const MagicEffect* m = effect(a.effect);
    if (a.effect != b.effect || !m || a.conditions.size() != b.conditions.size()) {
        return false;
    }
    for (size_t i = 0; i < a.conditions.size(); ++i) {
        const ConditionItem &x = a.conditions[i], &y = b.conditions[i];
        if (x.function != y.function || x.param1 != y.param1 || x.param2 != y.param2 || x.value != y.value) {
            return false;
        }
    }
    return ((m->flags & effect_no_magnitude) || a.magnitude == b.magnitude) && ((m->flags & effect_no_area) || a.area == b.area) &&
           ((m->flags & effect_no_duration) || a.duration == b.duration);
}

float Magic::relaunch_interval(uint32_t projectile) const {
    const forms::FormEntry* e = m_db->find(projectile);
    return e ? real(Form(*m_db, *e), "DATA.relaunch_interval") : 0.0f;
}

float Magic::dual_effectiveness(float cost) const {
    return m_settings->number("fMagicDualCastingEffectivenessMult", 0.0f) * cost +
           m_settings->number("fMagicDualCastingEffectivenessBase", 2.2f);
}

uint32_t Magic::create_enchantment(std::vector<Effect> effects, uint32_t cast_type, int32_t cost) const {
    for (const auto& [form, c] : m_created) {
        if (c.cast_type == cast_type && c.cost == cost && c.effects.size() == effects.size() &&
            std::equal(c.effects.begin(), c.effects.end(), effects.begin(), [](const Effect& a, const Effect& b) {
                return a.effect == b.effect && a.magnitude == b.magnitude && a.duration == b.duration;
            })) {
            return form;
        }
    }
    const uint32_t form = m_next_created++;
    m_created[form] = {std::move(effects), cast_type, cost};
    return form;
}

uint32_t Magic::base_enchantment(uint32_t ench) const {
    const forms::FormEntry* e = m_db->find(ench);
    if (!e) {
        return ench;
    }
    const Form f(*m_db, *e);
    const Value* base = f.get("ENIT.base_enchantment");
    return base && base->kind == Value::Kind::form && base->form ? base->form : ench;
}

std::vector<Effect> Magic::effects_of(uint32_t form) const {
    if (const auto it = m_created.find(form); it != m_created.end()) {
        return it->second.effects;
    }
    std::vector<Effect> out;
    if (const forms::FormEntry* e = m_db->find(form)) {
        for (const forms::EffectEntry& fx : forms::effects(Form(*m_db, *e))) {
            out.push_back({fx.effect, fx.magnitude, fx.area, fx.duration});
        }
    }
    return out;
}

float Magic::effect_cost(const Effect& e) const {
    const MagicEffect* m = effect(e.effect);
    if (!m) {
        return 0;
    }
    const float magnitude = (m->flags & effect_no_magnitude) ? 0.0f : e.magnitude;
    const float duration = (m->flags & effect_no_duration) ? 0.0f : float(e.duration);
    const float area = (m->flags & effect_no_area) ? 0.0f : float(e.area);
    const float d = duration >= 1.0f && m->casting_type != 2
                        ? duration * m_settings->number("fMagicDurMagBaseCostMult", 0.1f)
                        : 1.0f;
    float cost = m->base_cost * std::max(area * m_settings->number("fMagicAreaBaseCostMult", 0.15f), 1.0f) *
                 std::pow(std::max(magnitude, 1.0f) * d, m_settings->number("fMagicCostScale", 1.1f));
    if (m->delivery == 2) {
        cost *= m_settings->number("fMagicRangeTargetCostMult", 1.0f);
    }
    return cost;
}

float Magic::cost(std::span<const Effect> effects, bool manual, int32_t manual_cost) const {
    if (manual) {
        return float(manual_cost);
    }
    float total = 0;
    for (const Effect& e : effects) {
        const MagicEffect* m = effect(e.effect);
        if (m && !(m->flags & effect_no_cost)) {
            total += effect_cost(e);
        }
    }
    return std::max(total, 0.0f);
}

float Magic::enchantment_cost(uint32_t ench) const {
    if (const auto it = m_created.find(ench); it != m_created.end()) {
        return float(it->second.cost);
    }
    const forms::FormEntry* e = m_db->find(ench);
    if (!e) {
        return 0;
    }
    const Form f(*m_db, *e);
    std::vector<Effect> effects;
    for (const forms::EffectEntry& fx : forms::effects(f)) {
        effects.push_back({fx.effect, fx.magnitude, fx.area, fx.duration});
    }
    return cost(effects, (integer(f, "ENIT.flags") & 1) != 0, int32_t(integer(f, "ENIT.enchantment_cost")));
}

uint32_t Magic::enchantment_cast_type(uint32_t ench) const {
    if (const auto it = m_created.find(ench); it != m_created.end()) {
        return it->second.cast_type;
    }
    const forms::FormEntry* e = m_db->find(ench);
    return e ? uint32_t(integer(Form(*m_db, *e), "ENIT.cast_type")) : 0;
}

} // namespace spud::game
