#include "core/game_random.hpp"
#include "game/game.hpp"
#include "game/quests.hpp"
#include "game/scenes.hpp"

#include "core/log.hpp"
#include "formats/forms_typed.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <random>

namespace spud::game {

namespace {

using forms::Form;
using forms::Value;

constexpr forms::Sig sig(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
           uint32_t(uint8_t(s[3])) << 24;
}


uint32_t form_of(const Value* v) { return v && v->kind == Value::Kind::form ? v->form : 0; }
// RACE's big DATA: other fields of the record go by that signature too, and the last of a layered form's isn't always it.
const Value* race_data(const Form& race) {
    const Value* out = nullptr;
    for (const Value* v : race.all("DATA")) {
        out = v->member("flight_radius") ? v : out;
    }
    return out;
}
double real(const Value* v, double fallback = 0) {
    return !v ? fallback : v->kind == Value::Kind::real ? v->real : v->kind == Value::Kind::integer ? double(v->integer) : fallback;
}
int64_t integer(const Value* v, int64_t fallback = 0) {
    return v && v->kind == Value::Kind::integer ? v->integer : fallback;
}

} // namespace

Game::Game(const std::filesystem::path& forms_file)
    : m_db(std::make_unique<forms::Database>(forms_file)), m_settings(*m_db), m_items(*m_db),
      m_magic(*m_db, m_settings), m_conditions(*m_db, m_world), m_camera_paths(*m_db, m_conditions), m_leveled(*m_db, m_settings) {
    m_conditions.set_facts(this);
    m_conditions.created_base = [this](uint32_t ref) { return placed_base(ref); };
    m_character = std::make_unique<Character>(*this, *m_db, m_settings, m_magic, m_conditions);
    CharacterEvents& events = m_character->events;
    events.frame = [this](float dt) { update_magic(dt); };
    m_projectiles.on_magic_hit = [this](const MagicHit& h) {
        SpellImpact hit;
        hit.item = m_magic.spell(h.spell);
        hit.target = h.target;
        hit.location = h.position;
        hit.power = h.effectiveness;
        hit.dual = h.dual_cast;
        hit.area = h.explosion != 0;
        m_casters.caster(h.caster, uint8_t(h.casting_source)).projectile_hit(hit);
    };
    m_projectiles.on_ward = [this](const MagicHit& h) {
        const Spell* s = m_magic.spell(h.spell);
        const SpellEffect* top = s ? m_magic.costliest(*s, true) : nullptr;
        const MagicEffect* m = top ? m_magic.effect(top->effect) : nullptr;
        return s && magic_target(h.target).check_ward(*s, nullptr, m ? m_magic.relaunch_interval(m->projectile) : 0.0f, m_casters.ward_passes, h.caster);
    };
    events.notify = [this](const std::string& text, const std::string& sound) { notify(text, sound); };
    events.skill_notice = [this](const std::string& skill, int level, int player_level, float xp) {
        Notice n;
        n.skill = skill;
        n.skill_level = level;
        n.player_level = player_level;
        n.xp = xp;
        m_notices.push_back(std::move(n));
    };
    events.random_percent = [] {
        static std::mt19937 random{session_seed()};
        return std::uniform_real_distribution<float>(0.0f, 100.0f)(random);
    };
    m_character->values().derive = [this](int av) {
        return av == av_inventory_weight ? encumbrance() : total_armor();
    };
    const forms::FormEntry* e = m_db->find(k_player_base);
    if (!e) {
        return;
    }
    m_player.inventory.set_listener(this, k_player_ref);
    const Form f(*m_db, *e);
    m_player.name = f.name();
    for (const forms::FactionMembership& m : forms::npc_factions(f)) {
        m_player_factions.insert(m.faction);
    }
    for (const forms::ItemCount& c : forms::container_items(f)) {
        std::vector<Rolled> rolled;
        m_leveled.roll(c.item, int32_t(c.count), uint16_t(m_character->level()), rolled);
        for (const Rolled& r : rolled) {
            m_player.inventory.add(r.form, r.count, r.extra);
        }
    }
    // The default outfit is on: a game started straight in a cell keeps it, and a new game's script takes it off.
    const forms::FormEntry* outfit = m_db->find(form_of(f.get("DOFT")));
    const Value* items = outfit ? Form(*m_db, *outfit).get("INAM") : nullptr;
    for (size_t i = 0; items && i < items->items.size(); ++i) {
        std::vector<Rolled> rolled;
        m_leveled.roll(items->items[i].form, 1, uint16_t(m_character->level()), rolled);
        for (const Rolled& r : rolled) {
            m_player.inventory.add(r.form, r.count, r.extra);
            for (Stack& s : m_player.inventory.stacks()) {
                if (s.form == r.form) {
                    s.worn = Worn::worn;
                }
            }
        }
    }
}

Ai& Game::ai() {
    if (!m_ai) {
        m_ai = std::make_unique<Ai>(*this);
    }
    return *m_ai;
}

FurnitureKind Game::furniture_kind(uint32_t base) {
    const auto [it, fresh] = m_furniture_kinds.try_emplace(base, FurnitureKind::none);
    if (!fresh) {
        return it->second;
    }
    const forms::FormEntry* e = m_db->find(base);
    if (!e) {
        return FurnitureKind::none;
    }
    if (e->type == sig("IDLM")) {
        it->second = FurnitureKind::idle_marker;
    } else if (e->type == sig("FURN")) {
        const Form f(*m_db, *e);
        const int64_t markers = integer(f.get("MNAM"));
        const std::vector<uint32_t> keywords = f.keywords();
        const uint32_t roll = keyword("FurnitureBedRoll");
        if (markers & 0x80000000) { // sleep markers
            it->second = std::find(keywords.begin(), keywords.end(), roll) != keywords.end() ? FurnitureKind::bedroll
                                                                                            : FurnitureKind::bed;
        } else if ((markers & 0xFFFFFF) && integer(f.get("WBDT.bench_type")) == 0) { // any other with a marker
            it->second = FurnitureKind::chair;
        } else if (integer(f.get("WBDT.bench_type")) == 0) {
            it->second = FurnitureKind::work;
        }
    }
    return it->second;
}

// The idle tree (IDLE records): a root is keyed to the furniture by HasKeyword (560) or GetIsID (72) on the target, and under
// it Enter asks for GetSitting (159) 2 with entry type (631) 0, EnterInstant for 631 1.
const FurnitureEvents& Game::furniture_events(uint32_t base) {
    if (!m_idle_roots_read) {
        m_idle_roots_read = true;
        const auto number = [](const forms::Value* v) {
            return !v ? 0.0 : v->kind == forms::Value::Kind::real ? v->real : double(v->integer);
        };
        std::unordered_map<uint32_t, size_t> roots;
        for (const forms::FormEntry* e : m_db->of_type(sig("IDLE"))) {
            const Form f(*m_db, *e);
            for (const forms::Value* c : f.all("condition")) {
                const int64_t function = integer(c->member("function"));
                if ((function == 560 || function == 72) && integer(c->member("run_on")) == 1) {
                    roots[e->form_id] = m_idle_roots.size();
                    m_idle_roots.push_back({function, integer(c->member("param1")), {}});
                    break;
                }
            }
        }
        for (const forms::FormEntry* e : m_db->of_type(sig("IDLE"))) {
            const Form f(*m_db, *e);
            const forms::Value* related = f.get("ANAM");
            const forms::Value* parent = related ? related->member("parent") : nullptr;
            const auto root = parent ? roots.find(parent->form) : roots.end();
            const forms::Value* event = f.get("ENAM");
            if (root == roots.end() || !event || event->text.empty()) {
                continue;
            }
            int sitting = -1, entry = -1;
            for (const forms::Value* c : f.all("condition")) {
                const int64_t function = integer(c->member("function"));
                if (integer(c->member("operator")) != 0) {
                    continue;
                }
                sitting = function == 159 ? int(number(c->member("value"))) : sitting;
                entry = function == 631 ? int(number(c->member("value"))) : entry;
            }
            FurnitureEvents& out = m_idle_roots[root->second].events;
            if (entry == 1) {
                out.instant = event->text;
            } else if (sitting == 2) {
                out.enter = event->text;
            }
        }
    }
    const auto [it, fresh] = m_furniture_events.try_emplace(base);
    if (fresh) {
        if (const forms::FormEntry* e = m_db->find(base)) {
            const std::vector<uint32_t> keywords = Form(*m_db, *e).keywords();
            for (const IdleRoot& r : m_idle_roots) {
                const bool match = r.function == 72 ? uint32_t(r.param) == base
                                                    : std::find(keywords.begin(), keywords.end(), uint32_t(r.param)) != keywords.end();
                if (match && !r.events.enter.empty() && !r.events.enter.starts_with("IdleChair")) {
                    it->second = r.events;
                    break;
                }
            }
        }
    }
    return it->second;
}

bool Game::furniture_has_keyword(uint32_t base, std::string_view keyword_id) const {
    const forms::FormEntry* e = m_db->find(base);
    const uint32_t k = keyword(keyword_id);
    if (!e || !k) {
        return false;
    }
    const std::vector<uint32_t> keywords = Form(*m_db, *e).keywords();
    return std::ranges::find(keywords, k) != keywords.end();
}

bool Game::furniture_bench(uint32_t base) const {
    const forms::FormEntry* e = m_db->find(base);
    return e && integer(Form(*m_db, *e).get("WBDT.bench_type")) != 0;
}

uint32_t Game::space_of_cell(uint32_t cell) const {
    const forms::FormEntry* e = m_db->find(cell);
    return e && e->parent ? e->parent : cell;
}

NavMeshes& Game::navmeshes() {
    if (!m_navmeshes) {
        m_navmeshes = std::make_unique<NavMeshes>(*m_db);
    }
    return *m_navmeshes;
}

uint32_t Game::keyword(std::string_view editor_id) const {
    if (m_keywords.empty()) {
        for (const forms::FormEntry* e : m_db->of_type(sig("KYWD"))) {
            m_keywords.emplace(m_db->editor_id(*e), e->form_id);
        }
    }
    const auto it = m_keywords.find(std::string(editor_id));
    return it == m_keywords.end() ? 0 : it->second;
}

int32_t Game::base_value(const Item& item) const {
    if (item.kind != ItemKind::potion) {
        return item.value;
    }
    return int32_t(m_magic.cost(item.effects, (item.effect_flags & potion_manual_calc) != 0, item.value));
}

int32_t Game::value(const Stack& stack) const {
    const Item* item = m_items.find(stack.form);
    if (!item) {
        return 0;
    }
    const float v6 = m_settings.number("fHealthDataValue6", 1.6f);
    const float health = std::max(stack.extra.health, 1.0f);
    float v = float(base_value(*item));
    if (v6 > 1.0f) {
        v *= (health - 1.0f) / (v6 - 1.0f) + 1.0f;
    }
    v = v > 0.0f && v < 1.0f ? 1.0f : std::trunc(v);
    const uint32_t enchantment = item->enchantment ? item->enchantment : stack.extra.enchantment;
    if (!enchantment || v <= 0.0f) {
        return int32_t(v);
    }
    const float cost = m_magic.enchantment_cost(enchantment);
    if (m_magic.enchantment_cast_type(enchantment) == 0) {
        return int32_t(v) + int32_t(std::floor(cost + 0.5f)); // constant effect
    }
    const float charge = item->enchantment_amount ? float(item->enchantment_amount) : float(stack.extra.max_charge);
    return int32_t(v + m_settings.number("fEnchantmentPointsMult", 0.4f) * charge +
                   m_settings.number("fEnchantmentEffectPointsMult", 0.4f) * cost);
}

std::string Game::display_name(const Stack& stack) const {
    const Item* item = m_items.find(stack.form);
    std::string name = !stack.extra.name.empty() ? stack.extra.name : item ? item->name : std::string();
    const float health = stack.extra.health;
    if (!item || std::abs(health - 1.0f) <= 0.001f || health <= 0.0f) {
        return name;
    }
    // The last of the six thresholds the health reaches.
    static constexpr float k_defaults[6] = {1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f};
    int level = 0;
    for (int i = 1; i <= 6; ++i) {
        if (m_settings.number(std::format("fHealthDataValue{}", i), k_defaults[i - 1]) <= health) {
            level = i;
        }
    }
    if (level == 0) {
        return name;
    }
    const char* kind = item->kind == ItemKind::weapon ? "Weap" : "Armo";
    return std::format("{} ({})", name, m_settings.text(std::format("sHealthDataPrefix{}{}", kind, level)));
}

bool Game::listed(const Stack& stack) const {
    const Item* item = m_items.find(stack.form);
    if (!item || stack.form == k_gold || item->name.empty()) {
        return false;
    }
    if (item->kind == ItemKind::weapon) {
        return !(item->weapon_flags & 0x80); // Non-Playable
    }
    if (item->kind == ItemKind::ammo) {
        return !(item->weapon_flags & 0x2);
    }
    if (item->kind == ItemKind::armor) {
        return !(item->armor_flags & 0x4); // Non-Playable
    }
    return true;
}

float Game::weapon_damage(const Stack& stack) const {
    const Item* item = m_items.find(stack.form);
    if (!item) {
        return 0;
    }
    const Settings& s = m_settings;
    const float base = float(item->damage) * s.number("fDamageWeaponMult", 1.0f);
    if (item->kind == ItemKind::ammo) {
        return perk_value(0, perk_value(35, base, {stack.form, 0}), {stack.form, 0});
    }
    if (item->kind != ItemKind::weapon) {
        return 0;
    }
    float temper = 0;
    if (item->animation_type != 8) { // a staff takes no tempering
        temper = m_crafting.improvement(*item, std::max(stack.extra.health, 1.0f));
    }
    float multiplier = 1;
    if (item->skill >= k_first_skill && item->skill < k_first_skill + k_skill_count) {
        const float low = s.number("fDamagePCSkillMin", 1.0f), high = s.number("fDamagePCSkillMax", 1.5f);
        multiplier = (high - low) * std::clamp(m_character->skill(item->skill), 0.0f, 100.0f) * 0.01f + low;
    }
    // Melee weapons add actor value 34 MeleeDamage, unarmed 35; both 0 for the player's stats here.
    return perk_value(0, perk_value(35, (base + temper) * multiplier, {stack.form, 0}), {stack.form, 0});
}

float Game::armor_rating(const Stack& stack) const {
    const Item* item = m_items.find(stack.form);
    if (!item || item->kind != ItemKind::armor || item->armor_type == 2) {
        return 0;
    }
    const float low = m_settings.number("fArmorRatingPCBase", 1.0f), high = m_settings.number("fArmorRatingPCMax", 1.4f);
    const float skill = m_character->skill(item->armor_type == 0 ? light_armor : heavy_armor);
    const float factor = (high - low) * skill * 0.01f + low;
    const float temper = m_crafting.improvement(*item, std::max(stack.extra.health, 1.0f));
    return perk_value(85, std::ceil((float(item->armor_rating) / 100.0f + temper) * factor), {stack.form});
}

int32_t Game::total_damage() const {
    const auto round = [](float v) { return int32_t(std::floor(v + 0.5f)); };
    int32_t total = 0;
    const Stack* ammo = nullptr;
    bool ranged = false;
    for (const Stack& s : m_player.inventory.stacks()) {
        const Item* item = m_items.find(s.form);
        if (!item || s.worn == Worn::none) {
            continue;
        }
        if (item->kind == ItemKind::weapon) {
            total += round(weapon_damage(s)) * (s.worn == Worn::both ? 2 : 1);
            ranged = ranged || item->animation_type >= 7;
        } else if (item->kind == ItemKind::ammo) {
            ammo = &s;
        }
    }
    return total + (ranged && ammo ? round(weapon_damage(*ammo)) : 0);
}

float Game::total_armor() const {
    float total = 0;
    for (const Stack& s : m_player.inventory.stacks()) {
        if (s.worn != Worn::none) {
            total += armor_rating(s);
        }
    }
    return total;
}

namespace {

constexpr std::string_view k_value_prefix = "<font face='$EverywhereMediumFont' size='20' color='#FFFFFF'>";
constexpr std::string_view k_value_suffix = "</font>";

// Strips [...] blocks: [SURV=text] is survival mode's only, the rest vanish.
std::string strip_blocks(const std::string& text, bool survival) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '[') {
            const size_t end = text.find(']', i);
            if (end != std::string::npos) {
                constexpr std::string_view k_tag = "[SURV=";
                if (survival && text.compare(i, k_tag.size(), k_tag) == 0) {
                    out.append(text, i + k_tag.size(), end - i - k_tag.size());
                }
                i = end;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

// Each <tag> of a description, wrapped.
std::string wrap_tags(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '<') {
            if (const size_t end = text.find('>', i); end != std::string::npos) {
                out += k_value_prefix;
                out += text.substr(i + 1, end - i - 1);
                out += k_value_suffix;
                i = end;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

} // namespace

std::string Game::effect_lines(std::span<const Effect> effects) const {
    std::string out;
    for (const Effect& e : effects) {
        const MagicEffect* m = m_magic.effect(e.effect);
        if (!m || m->description.empty()) {
            continue;
        }
        std::string line;
        const std::string& d = m->description;
        for (size_t i = 0; i < d.size(); ++i) {
            const size_t end = d[i] == '<' ? d.find('>', i) : std::string::npos;
            if (end == std::string::npos) {
                line += d[i];
                continue;
            }
            const std::string tag = d.substr(i + 1, end - i - 1);
            i = end;
            std::string value;
            if (tag == "mag") {
                if (!(m->flags & effect_no_magnitude)) {
                    // Only the combat health regen multiplier (134) is held as a fraction.
                    value = std::format("{}", int64_t(e.magnitude * (m->actor_value == 134 ? 100.0f : 1.0f)));
                }
            } else if (tag == "dur") {
                if (!(m->flags & effect_no_duration)) {
                    value = std::format("{}", e.duration);
                }
            } else if (tag == "area") {
                if (!(m->flags & effect_no_area)) {
                    value = std::format("{}", e.area);
                }
            } else {
                value = tag;
            }
            line += k_value_prefix;
            line += value;
            line += k_value_suffix;
        }
        if (!out.empty()) {
            out += ' ';
        }
        out += line;
    }
    return out;
}

std::string Game::effects_text(const Stack& stack) const {
    const Item* item = m_items.find(stack.form);
    if (!item) {
        return {};
    }
    std::string out;
    if (item->kind == ItemKind::armor || item->kind == ItemKind::weapon || item->kind == ItemKind::ammo ||
        item->kind == ItemKind::scroll) {
        out = wrap_tags(item->description);
    }
    if (out.empty()) {
        if (item->kind == ItemKind::potion || item->kind == ItemKind::scroll) {
            out = effect_lines(item->effects);
        } else {
            const uint32_t ench = stack.extra.enchantment ? stack.extra.enchantment : item->enchantment;
            if (ench) {
                out = effect_lines(m_magic.effects_of(ench));
            }
        }
    }
    return strip_blocks(out, survival());
}

float Game::slot_rating(const Item& item) const {
    // What the pieces worn in the item's slots rate, each once.
    float total = 0;
    for (const Stack& s : m_player.inventory.stacks()) {
        const Item* worn = s.worn != Worn::none ? m_items.find(s.form) : nullptr;
        if (worn && worn->kind == ItemKind::armor && (worn->biped_slots & item.biped_slots)) {
            total += std::floor(armor_rating(s) + 0.5f);
        }
    }
    return total;
}

int Game::spell_skill(uint32_t spell) const {
    const std::vector<Effect> effects = m_magic.effects_of(spell);
    const MagicEffect* m = effects.empty() ? nullptr : m_magic.effect(effects[0].effect);
    return m ? m->magic_skill : -1;
}

int Game::potion_type(const Item& item) const {
    // By the costliest effect's actor value: Health 24, 27, 155 give 0, Magicka 25, 28, 156 give 1, the rest 2.
    const Effect* top = nullptr;
    float cost = -1;
    for (const Effect& e : item.effects) {
        if (const float c = m_magic.effect_cost(e); c > cost) {
            cost = c;
            top = &e;
        }
    }
    const MagicEffect* m = top ? m_magic.effect(top->effect) : nullptr;
    const int av = m ? m->actor_value : -1;
    return av == 24 || av == 27 || av == 155 ? 0 : av == 25 || av == 28 || av == 156 ? 1 : 2;
}

std::string Game::soul_name(uint8_t soul) const {
    static constexpr const char* k_names[] = {"sSoulLevelNamePetty", "sSoulLevelNameLesser", "sSoulLevelNameCommon",
                                              "sSoulLevelNameGreater", "sSoulLevelNameGrand"};
    return soul >= 1 && soul <= 5 ? m_settings.text(k_names[soul - 1]) : std::string();
}

std::string Game::spell_text(uint32_t spell) const { return strip_blocks(effect_lines(m_magic.effects_of(spell)), survival()); }

uint32_t Game::filter_flag(const Stack& stack) const {
    const Item* item = m_items.find(stack.form);
    uint32_t flag = 512;
    if (item) {
        switch (item->kind) {
        case ItemKind::scroll: flag = 16; break;
        case ItemKind::armor: flag = 4; break;
        case ItemKind::book: flag = 128; break;
        case ItemKind::ingredient: flag = 64; break;
        case ItemKind::weapon:
        case ItemKind::ammo: flag = 2; break;
        case ItemKind::key: flag = 256; break;
        case ItemKind::potion: flag = item->food() ? 32 : 8; break;
        default: break;
        }
    }
    return flag | (stack.favorite ? 1u : 0u);
}

std::string Game::skill_name(int actor_value) const {
    const forms::FormEntry* e = m_db->find(0x00000446 + uint32_t(actor_value)); // AVIF
    return e ? std::string(Form(*m_db, *e).name()) : std::string();
}

float Game::npc_skill(uint32_t npc, int actor_value) const {
    const forms::FormEntry* e = m_db->find(npc);
    if (!e) {
        return 0;
    }
    const Form f(*m_db, *e);
    const Value* v = f.get("player_skills.skill_values");
    const auto i = size_t(actor_value - k_first_skill);
    return v && v->kind == Value::Kind::list && i < v->items.size() ? float(v->items[i].integer) : 0.0f;
}

float Game::skill_percent(int actor_value) const {
    const float percent = m_character->skill_percent(actor_value);
    return std::isnan(percent) ? 0.0f : percent;
}

float Game::encumbrance() const {
    // a worn piece's weight goes through entry point 32.
    float total = m_player.inventory.weight(m_items);
    const bool survive = survival();
    for (const Stack& s : m_player.inventory.stacks()) {
        const Item* item = m_items.find(s.form);
        if (item && s.worn != Worn::none) {
            total += perk_value(ep_armor_weight, item->weight, {s.form}) - item->weight;
        }
        if (item && survive) {
            total += item->survival_weight * float(s.count);
        }
    }
    return total;
}

float Game::carry_weight() const { return perk_value(ep_max_carry_weight, m_character->values().current(av_carry_weight)); }

Inventory& Game::container(uint32_t ref) {
    if (const auto it = m_containers.find(ref); it != m_containers.end()) {
        return it->second;
    }
    Inventory& inv = m_containers[ref];
    const forms::FormEntry* e = m_db->find(ref);
    if (!e) {
        return inv;
    }
    const Form f(*m_db, *e);
    const forms::FormEntry* base = e->type == sig("NPC_") ? e : m_db->find(form_of(f.get("NAME")));
    if (!base) {
        return inv;
    }
    for (const forms::ItemCount& c : forms::container_items(Form(*m_db, *base))) {
        std::vector<Rolled> rolled;
        m_leveled.roll(c.item, int32_t(c.count), uint16_t(m_character->level()), rolled);
        for (const Rolled& r : rolled) {
            inv.add(r.form, r.count, r.extra);
        }
    }
    inv.set_listener(this, ref);
    return inv;
}

const std::array<float, 6>* Game::activatable(uint32_t base) {
    auto [it, fresh] = m_bounds.try_emplace(base);
    std::array<float, 6>& box = it->second;
    if (fresh) {
        const forms::FormEntry* e = m_db->find(base);
        const forms::Sig t = e ? e->type : 0;
        const bool any = t == sig("CONT") || t == sig("FURN") || t == sig("DOOR") || t == sig("FLOR") ||
                         t == sig("TREE") || t == sig("ACTI") || m_items.find(base);
        if (any) {
            const Form f(*m_db, *e);
            if (const Value* b = f.get("OBND")) {
                for (size_t i = 0; i < 6; ++i) {
                    static constexpr const char* k_names[6] = {"x1", "y1", "z1", "x2", "y2", "z2"};
                    box[i] = float(integer(b->member(k_names[i])));
                }
            }
        }
    }
    return box == std::array<float, 6>{} ? nullptr : &box;
}

uint32_t Game::place(const Stack& stack, const std::array<float, 3>& position) {
    const uint32_t id = m_next_placed++;
    m_placed[id] = {stack, position};
    return id;
}

Target Game::inspect(uint32_t ref) {
    Target t;
    t.ref = ref;
    if (m_taken.contains(ref)) {
        return t;
    }
    if (const auto it = m_placed.find(ref); it != m_placed.end()) {
        t.base = it->second.stack.form;
        t.count = it->second.stack.count;
        t.item = m_items.find(t.base);
        if (t.item) {
            t.kind = TargetKind::item;
            t.name = t.item->name;
            t.verb = m_settings.text(t.item->kind == ItemKind::book ? "sRead" : "sTake");
        }
        return t;
    }
    const forms::FormEntry* e = m_db->find(ref);
    if (!e || e->type != sig("REFR")) {
        return t;
    }
    const Form f(*m_db, *e);
    t.base = form_of(f.get("NAME"));
    const forms::FormEntry* base = m_db->find(t.base);
    if (!base) {
        return t;
    }
    t.count = int32_t(std::max<int64_t>(integer(f.get("XCNT"), 1), 1));
    t.owner = owner_of(ref);
    const uint32_t holder = lock_holder(ref);
    const Form locked_by(*m_db, *m_db->find(holder));
    if (const Value* lock = locked_by.get("XLOC"); lock && !m_unlocked.contains(holder)) {
        // Any XLOC is a locked lock: the game calls SetLocked(true) after reading it, and
        // no plugin sets bit 0 of the flags. Bit 2 levels it with the zone, or the player without one.
        const int64_t flags = integer(lock->member("flags"));
        int64_t n = integer(lock->member("level"));
        if (n != 255 && (flags & 4)) {
            n = std::min<int64_t>(n + int64_t(m_settings.number("fLeveledLockMult", 1.5f)) * m_character->level(), 99);
        }
        t.lock_level = uint8_t(std::max<int64_t>(n, 1));
        t.key = form_of(lock->member("key"));
    }
    if (const RefChanges* c = find_changes(holder)) {
        // SetLockLevel makes a lock, left unlocked: Lock turns it.
        if (c->locked && *c->locked && !t.lock_level) {
            t.lock_level = 1;
        }
        if (c->locked && !*c->locked) {
            t.lock_level = 0;
        }
        if (c->lock_level && t.lock_level) {
            t.lock_level = std::max<uint8_t>(*c->lock_level, 1);
        }
    }
    const Form b(*m_db, *base);
    t.name = b.name();
    t.item = m_items.find(t.base);
    t.steal = t.owner && !may_take(t.owner, t.item ? base_value(*t.item) : 0);
    if (t.item) {
        if (t.item->kind == ItemKind::book && (t.item->book_flags & book_cant_take) &&
            !(t.item->book_flags & book_teaches_spell)) {
            t.kind = TargetKind::item;
            t.verb = m_settings.text("sRead");
            return t;
        }
        t.kind = TargetKind::item;
        const bool tome = t.item->book_flags & book_teaches_spell;
        t.verb = m_settings.text(t.item->kind == ItemKind::book && !tome ? "sRead" : t.steal ? "sSteal" : "sTake");
    } else if (base->type == sig("CONT")) {
        t.kind = TargetKind::container;
        t.verb = m_settings.text(t.steal ? "sStealFrom" : t.locked() ? "sUnlock" : "sSearch");
        // A third line: Empty, else the lock's level.
        if (container(ref).empty()) {
            t.detail = m_settings.text("sEmpty");
        } else if (t.locked()) {
            static constexpr const char* k_levels[6] = {"sLockLevelNameVeryEasy", "sLockLevelNameEasy",
                                                        "sLockLevelNameAverage", "sLockLevelNameHard",
                                                        "sLockLevelNameVeryHard", "sLockLevelNameImpossible"};
            t.detail = m_settings.text(k_levels[lock_index(t.lock_level)]);
        }
    } else if (base->type == sig("FURN")) {
        t.bed = integer(b.get("MNAM")) & 0x80000000;
        // The text: a bench uses, a sleep marker sleeps, a sit marker sits. An owned bed says so.
        t.kind = TargetKind::furniture;
        const bool bench = integer(b.get("WBDT.bench_type")) != 0;
        t.verb = m_settings.text(bench ? "sUse" : t.bed ? "sFurnitureSleep" : furniture_kind(t.base) == FurnitureKind::chair && !furniture_has_keyword(t.base, "FurnitureSpecial") ? "sSit" : "sUse");
        if (t.bed && t.steal) {
            t.name += std::format(" ({})", m_settings.text("sOwned"));
        }
    } else if (base->type == sig("ACTI")) {
        // Only one a script listens on does anything here; RNAM names its verb.
        if (b.get("VMAD") && !t.name.empty()) {
            t.kind = TargetKind::activator;
            const Value* text = b.get("RNAM");
            t.verb = text && !text->text.empty() ? text->text : m_settings.text("sActivate");
        }
    } else if (base->type == sig("DOOR")) {
        t.kind = TargetKind::door;
        t.verb = m_settings.text(t.locked() ? "sUnlock" : "sOpen");
        if (const std::string place = door_destination_name(ref); !place.empty()) {
            t.name = place;
        }
    } else if (base->type == sig("FLOR") || base->type == sig("TREE")) {
        if (form_of(b.get("PFIG")) && !m_harvested.contains(ref)) {
            t.kind = TargetKind::flora;
            const Value* text = b.get("RNAM");
            t.verb = t.steal                            ? m_settings.text("sSteal")
                     : text && !text->text.empty()      ? text->text
                                                        : m_settings.text("sHarvest");
        }
    }
    return t;
}

constexpr uint32_t k_race_allow_pickpocket = 0x800000;

Target Game::inspect_actor(uint32_t ref, uint32_t npc, bool sneaking) {
    Target t;
    t.kind = TargetKind::actor;
    t.ref = ref;
    t.base = npc;
    t.name = display_name_of(npc);
    // The game's activate text: the dead are searched, and a third line says when there's nothing on them.
    if (ref_dead && ref_dead(ref)) {
        t.corpse = true;
        t.verb = m_settings.text("sSearch");
        if (container(ref).empty()) {
            t.detail = m_settings.text("sEmpty");
        }
        return t;
    }
    // The game's activate text: a live horse says Ride, or Steal when that is a crime, and never Talk or Pickpocket.
    if (mount_data(npc)) {
        t.mount = true;
        t.owner = owner_of(ref);
        t.steal = !may_take(t.owner, 0);
        t.verb = m_settings.text(t.steal ? "sSteal" : "sRide");
        return t;
    }
    // The game's activate text: a teammate talks to a sneaking player, others are a pocket if the race has
    // them, and with no dialogue there is nothing to say at all. An empty verb is no prompt and no activation.
    const bool teammate = ref_flag(ref, ref_teammate);
    if (sneaking && !teammate) {
        t.steal = (race_flags(npc) & k_race_allow_pickpocket) != 0;
        t.verb = t.steal ? m_settings.text("sPickpocket") : std::string();
    } else if (allows_dialogue(ref, npc)) {
        t.verb = m_settings.text("sTalk");
    }
    t.hostile = m_combat.fighter(ref).combat_target == k_player_ref;
    return t;
}

void Game::stock_corpse(uint32_t ref, uint32_t inventory_npc, const std::vector<uint32_t>& worn) {
    if (ref_flag(ref, ref_corpse_stocked)) {
        return;
    }
    set_ref_flag(ref, ref_corpse_stocked, true);
    Inventory& inv = container(ref);
    const auto roll = [&](uint32_t form, int32_t count) {
        std::vector<Rolled> rolled;
        m_leveled.roll(form, count, uint16_t(m_character->level()), rolled);
        for (const Rolled& r : rolled) {
            inv.add(r.form, r.count, r.extra);
        }
    };
    const forms::FormEntry* placed = m_db->find(ref);
    const uint32_t base = placed ? form_of(Form(*m_db, *placed).get("NAME")) : 0;
    if (const forms::FormEntry* e = m_db->find(inventory_npc)) {
        const Form npc(*m_db, *e);
        if (inventory_npc != base) { // a leveled actor or a template: container() only saw the reference's base
            for (const forms::ItemCount& c : forms::container_items(npc)) {
                roll(c.item, int32_t(c.count));
            }
        }
        if (const uint32_t death = form_of(npc.get("INAM"))) {
            roll(death, 1);
        }
    }
    for (const uint32_t item : worn) {
        if (std::ranges::none_of(inv.stacks(), [item](const Stack& s) { return s.form == item; })) {
            roll(item, 1);
        }
    }
}

uint32_t Game::race_flags(uint32_t npc) {
    const forms::FormEntry* race = m_db->find(m_conditions.race_of(npc)); // a template's, when traits come from it
    const Value* data = race ? race_data(Form(*m_db, *race)) : nullptr;
    const Value* flags = data ? data->member("flags") : nullptr;
    return flags ? uint32_t(flags->integer) : 0;
}

// the script's word, else the race's.
bool Game::allows_dialogue(uint32_t ref, uint32_t npc) {
    constexpr uint32_t k_race_allow_dialogue = 0x200000;
    return !ref_flag(ref, ref_no_dialogue) && (race_flags(npc) & k_race_allow_dialogue);
}

int Game::race_size(uint32_t npc) const {
    const forms::FormEntry* e = m_db->find(npc);
    const forms::FormEntry* race = e && e->type == sig("NPC_") ? m_db->find(form_of(Form(*m_db, *e).get("RNAM"))) : nullptr;
    const Value* data = race ? race_data(Form(*m_db, *race)) : nullptr;
    const Value* size = data ? data->member("size") : nullptr;
    return size ? int(size->integer) : 1;
}

std::optional<float> Game::flight_radius(uint32_t npc) const {
    const forms::FormEntry* e = m_db->find(npc);
    const forms::FormEntry* race = e && e->type == sig("NPC_") ? m_db->find(form_of(Form(*m_db, *e).get("RNAM"))) : nullptr;
    if (!race) {
        return std::nullopt;
    }
    const Form r(*m_db, *race);
    const Value* data = race_data(r);
    const Value* flags = data ? data->member("flags") : nullptr;
    if (!flags || !(flags->integer & 0x80)) {
        return std::nullopt;
    }
    return float(real(data->member("flight_radius")));
}

namespace {

// The skeleton's BBX is what the controller is built from; the NPC_'s bound is it to the nearest unit.
std::array<float, 3> bound_half(const Form& f) {
    const auto extent = [&](const char* lo, const char* hi) {
        return 0.5f * float(real(f.get(hi)) - real(f.get(lo)));
    };
    return {extent("OBND.x1", "OBND.x2"), extent("OBND.y1", "OBND.y2"), extent("OBND.z1", "OBND.z2")};
}

} // namespace

std::optional<MountData> Game::mount_data(uint32_t npc, bool tamed) const {
    const forms::FormEntry* e = m_db->find(npc);
    if (!e || e->type != sig("NPC_")) {
        return std::nullopt;
    }
    const Form f(*m_db, *e);
    const std::vector<uint32_t> keywords = f.keywords();
    const uint32_t horse = keyword("ActorTypeHorse");
    const bool is_horse = std::find(keywords.begin(), keywords.end(), horse) != keywords.end();
    if (!is_horse && !(tamed && flight_radius(npc))) {
        return std::nullopt;
    }
    const forms::FormEntry* race = m_db->find(form_of(f.get("RNAM")));
    if (!race) {
        return std::nullopt;
    }
    const Form r(*m_db, *race);
    const Value* data = race_data(r);
    if (!data) {
        return std::nullopt;
    }
    const auto vec = [&](const char* name) {
        std::array<float, 3> out{};
        if (const Value* v = data->member(name)) {
            out = {float(real(v->member("x"))), float(real(v->member("y"))), float(real(v->member("z")))};
        }
        return out;
    };
    MountData d;
    d.horse = is_horse;
    d.mount_offset = vec("mount_offset");
    d.dismount_offset = vec("dismount_offset");
    d.camera_offset = vec("mount_camera_offset");
    d.stamina = float(real(data->member("starting_stamina")));
    d.stamina_regen = float(real(data->member("stamina_regen")));
    d.half = bound_half(f);
    return d;
}

bool Game::race_has_keyword(uint32_t race, std::string_view keyword_id) const {
    const forms::FormEntry* e = m_db->find(race);
    if (!e) {
        return false;
    }
    const std::vector<uint32_t> keywords = Form(*m_db, *e).keywords();
    return std::find(keywords.begin(), keywords.end(), keyword(keyword_id)) != keywords.end();
}

std::array<float, 3> Game::body_half(uint32_t npc) {
    const auto [known, fresh] = m_body_halves.try_emplace(npc);
    if (fresh) {
        const forms::FormEntry* e = m_db->find(npc);
        known->second = e ? bound_half(Form(*m_db, *e)) : std::array<float, 3>{};
        if (known->second[2] <= 0.0f) {
            known->second = {23.0f, 17.5f, 64.0f}; // a templated NPC_ can have none: the game's fallback
        }
    }
    return known->second;
}

uint32_t Game::own_owner(uint32_t ref) const {
    if (const RefChanges* c = find_changes(ref); c && c->owner) {
        return *c->owner; // None took the record's away
    }
    const forms::FormEntry* e = m_db->find(ref);
    return e ? form_of(Form(*m_db, *e).get("XOWN")) : 0;
}

uint32_t Game::lock_holder(uint32_t ref) const {
    const auto has_lock = [this](uint32_t r, const Form& f) {
        const RefChanges* c = find_changes(r);
        return f.get("XLOC") || (c && (c->locked || c->lock_level));
    };
    const forms::FormEntry* e = m_db->find(ref);
    if (!e || e->type != sig("REFR")) {
        return ref;
    }
    const Form f(*m_db, *e);
    if (has_lock(ref, f)) {
        return ref;
    }
    const Value* link = f.get("XTEL");
    const uint32_t door = link ? form_of(link->member("door")) : 0;
    const forms::FormEntry* other = door ? m_db->find(door) : nullptr;
    return other && other->type == sig("REFR") && has_lock(door, Form(*m_db, *other)) ? door : ref;
}

// The door's activate text names where it leads: the cell of the door at the far end, and out of
// doors its world's, which a map-name region replaces in the game.
std::string Game::door_destination_name(uint32_t ref) const {
    const forms::FormEntry* e = m_db->find(ref);
    const Value* link = e ? Form(*m_db, *e).get("XTEL") : nullptr;
    const forms::FormEntry* far = link ? m_db->find(form_of(link->member("door"))) : nullptr;
    const forms::FormEntry* cell = far ? m_db->find(far->parent) : nullptr;
    if (!cell) {
        return {};
    }
    std::string name(Form(*m_db, *cell).name());
    if (const forms::FormEntry* world = name.empty() ? m_db->find(cell->parent) : nullptr) {
        name = std::string(Form(*m_db, *world).name());
    }
    return name;
}

uint32_t Game::owner_of(uint32_t ref) const {
    if (const uint32_t own = own_owner(ref)) {
        return own;
    }
    const forms::FormEntry* e = m_db->find(ref);
    return e ? own_owner(e->parent) : 0;
}

bool Game::may_take(uint32_t owner, int32_t value) const {
    if (!owner || owner == k_player_base || m_player_factions.contains(owner)) {
        return true;
    }
    const int favor = m_crime.favor_value(owner);
    return favor > 1 && value <= favor;
}

uint32_t Game::location_crime_faction(uint32_t ref) const {
    const forms::FormEntry* e = m_db->find(ref);
    const forms::FormEntry* cell = e ? m_db->find(e->parent) : nullptr;
    if (!cell) {
        return 0;
    }
    uint32_t location = form_of(Form(*m_db, *cell).get("XLCN"));
    for (int depth = 0; location && depth < 16; ++depth) {
        const forms::FormEntry* l = m_db->find(location);
        if (!l) {
            return 0;
        }
        const Form f(*m_db, *l);
        if (const uint32_t faction = form_of(f.get("FNAM"))) {
            return faction;
        }
        location = form_of(f.get("PNAM"));
    }
    return 0;
}

int Game::record_relationship(uint32_t a, uint32_t b) const {
    if (m_relationships.empty()) {
        static constexpr int k_ranks[9] = {4, 3, 2, 1, 0, -1, -2, -3, -4}; // the index in the record, as Papyrus sees it
        m_relationships.emplace(0, 0);
        for (const forms::FormEntry* e : m_db->of_type(sig("RELA"))) {
            const Form f(*m_db, *e);
            const Value* d = f.get("DATA");
            if (!d) {
                continue;
            }
            const uint32_t x = form_of(d->member("parent")), y = form_of(d->member("child"));
            const auto rank = size_t(integer(d->member("rank")));
            m_relationships[x < y ? uint64_t(x) << 32 | y : uint64_t(y) << 32 | x] = rank < 9 ? k_ranks[rank] : 0;
        }
    }
    const auto it = m_relationships.find(a < b ? uint64_t(a) << 32 | b : uint64_t(b) << 32 | a);
    return it == m_relationships.end() ? 0 : it->second;
}

bool Game::has_association(uint32_t a, uint32_t type, uint32_t b) const {
    for (const forms::FormEntry* e : m_db->of_type(sig("RELA"))) {
        const Form f(*m_db, *e);
        const Value* d = f.get("DATA");
        if (!d || form_of(d->member("association_type")) != type) {
            continue;
        }
        const uint32_t x = form_of(d->member("parent")), y = form_of(d->member("child"));
        if ((x == a && (!b || y == b)) || (y == a && (!b || x == b))) {
            return true;
        }
    }
    return false;
}

uint32_t Game::base_of(uint32_t ref) const {
    if (ref == 0x14) {
        return k_player_base;
    }
    const forms::FormEntry* e = m_db->find(ref);
    if (!e) { // made at run time: no record of its own
        if (const auto it = m_placed_forms.find(ref); it != m_placed_forms.end()) {
            return it->second;
        }
        const auto it = m_placed.find(ref);
        return it == m_placed.end() ? 0 : it->second.stack.form;
    }
    if (e->type == sig("NPC_")) {
        return ref;
    }
    return forms::placed_base(*m_db, *e);
}

// A unique NPC keeps its factions on its base, so a row it leaves is gone; anyone else's stays as -1 (ACBS unique bit).
static constexpr uint32_t k_acbs_unique = 0x20;

int32_t Game::faction_rank(uint32_t ref, uint32_t faction) {
    const RefChanges* c = find_changes(ref);
    if (c) {
        if (const auto it = c->factions.find(faction); it != c->factions.end()) {
            return it->second;
        }
    }
    const uint32_t base = base_of(ref);
    const forms::FormEntry* e = m_db->find(base);
    if (e) {
        for (const forms::FactionMembership& m : forms::npc_factions(forms::Form(*m_db, *e))) {
            if (m.faction == faction) {
                if (ref == k_player_ref && m_crime.expelled(faction)) {
                    return k_not_in_faction;
                }
                if (c && c->left_all_factions) {
                    return base_flag(base, k_acbs_unique) ? k_not_in_faction : k_left_faction;
                }
                return m.rank;
            }
        }
    }
    return k_not_in_faction;
}

void Game::set_faction_rank(uint32_t ref, uint32_t faction, int32_t rank) {
    rank = int8_t(rank); // a byte in the rows
    if (rank == k_left_faction && base_flag(base_of(ref), k_acbs_unique)) {
        rank = k_not_in_faction;
    }
    changes(ref).factions[faction] = rank;
    if (ref == k_player_ref) {
        set_player_faction(faction, rank >= 0);
    }
}

void Game::leave_faction(uint32_t ref, uint32_t faction) {
    if (crime_faction(ref) == faction) {
        changes(ref).crime_faction = 0;
    }
    set_faction_rank(ref, faction, k_left_faction);
}

void Game::leave_all_factions(uint32_t ref) {
    RefChanges& c = changes(ref);
    const bool unique = base_flag(base_of(ref), k_acbs_unique);
    for (auto& [faction, rank] : c.factions) {
        rank = unique ? k_not_in_faction : k_left_faction;
    }
    c.left_all_factions = true;
    if (ref == k_player_ref) {
        m_player_factions.clear();
    }
}

static int32_t use_state(const Agent* a, std::initializer_list<Activity> kinds) {
    if (!a || std::find(kinds.begin(), kinds.end(), a->activity) == kinds.end()) {
        return 0;
    }
    return a->use == UseState::going ? 2 : a->use == UseState::using_it ? 3 : a->use == UseState::leaving ? 4 : 0;
}

int32_t Game::sit_state(uint32_t ref) { return ref == k_player_ref ? m_player_sit : use_state(ai().agent(ref), {Activity::sit, Activity::eat_sitting}); }

int32_t Game::sleep_state(uint32_t ref) { return use_state(ai().agent(ref), {Activity::sleep}); }

static constexpr uint32_t k_acbs_ghost = 0x20000000;

bool Game::is_ghost(uint32_t ref) const {
    return ref_flag(ref, ref_ghost_set) ? ref_flag(ref, ref_ghost) : base_flag(base_of(ref), k_acbs_ghost);
}

void Game::set_ghost(uint32_t ref, bool ghost) {
    set_ref_flag(ref, ref_ghost_set, ghost != base_flag(base_of(ref), k_acbs_ghost));
    set_ref_flag(ref, ref_ghost, ghost);
}

uint32_t Game::crime_faction(uint32_t ref) const {
    if (const RefChanges* c = find_changes(ref); c && c->crime_faction) {
        return *c->crime_faction;
    }
    return m_crime.crime_faction_of(base_of(ref));
}

bool Game::base_flag(uint32_t npc, uint32_t flag) const {
    bool on = false;
    if (const forms::FormEntry* e = m_db->find(npc)) {
        const forms::Form f(*m_db, *e);
        const forms::Value* flags = f.get("ACBS.flags");
        on = flags && (uint32_t(flags->integer) & flag) != 0;
    }
    if (const auto it = m_base_flags.find(npc); it != m_base_flags.end()) {
        on = (on || (it->second.first & flag)) && !(it->second.second & flag);
    }
    return on;
}

void Game::set_base_flag(uint32_t npc, uint32_t flag, bool on) {
    auto& [set, cleared] = m_base_flags[npc];
    set = on ? set | flag : set & ~flag;
    cleared = on ? cleared & ~flag : cleared | flag;
}

void Game::set_player_faction(uint32_t faction, bool member) {
    if (member) {
        m_player_factions.insert(faction);
    } else {
        m_player_factions.erase(faction);
    }
}

std::vector<Rolled> Game::roll(uint32_t form) const {
    std::vector<Rolled> out;
    m_leveled.roll(form, 1, uint16_t(m_character->level()), out);
    return out;
}

int Game::lock_index(uint8_t level) const {
    static constexpr const char* k_names[5] = {"iLockLevelMaxVeryEasy", "iLockLevelMaxEasy", "iLockLevelMaxAverage",
                                               "iLockLevelMaxHard", "iLockLevelMaxVeryHard"};
    static constexpr float k_defaults[5] = {0, 25, 50, 75, 100};
    for (int i = 0; i < 5; ++i) {
        if (float(level) <= m_settings.number(k_names[i], k_defaults[i])) {
            return i;
        }
    }
    return 5;
}

LockNumbers Game::lock_numbers(int index) const {
    static constexpr const char* k_sweet[5] = {"fSweetSpotVeryEasy", "fSweetSpotEasy", "fSweetSpotAverage",
                                               "fSweetSpotHard", "fSweetSpotVeryHard"};
    static constexpr const char* k_partial[5] = {"fPartialPickVeryEasy", "fPartialPickEasy", "fPartialPickAverage",
                                                 "fPartialPickHard", "fPartialPickVeryHard"};
    static constexpr const char* k_break[5] = {"fLockpickBreakNovice", "fLockpickBreakApprentice", "fLockpickBreakAdept",
                                               "fLockpickBreakExpert", "fLockpickBreakMaster"};
    static constexpr float k_sweet_default[5] = {35, 17, 8, 5, 3};
    static constexpr float k_break_default[5] = {2, 1, 0.75f, 0.5f, 0.25f};
    LockNumbers n;
    if (index >= 5) {
        return n;
    }
    const Settings& s = m_settings;
    const float skill = std::clamp(m_character->skill(lockpicking), 0.0f, 100.0f);
    const float mult = perk_value(59, 1.0f);
    n.size = (s.number("fLockpickSkillSweetSpotMult", 0.012f) * skill + s.number("fLockpickSkillSweetSpotBase", 0.82f)) *
             mult * s.number(k_sweet[index], k_sweet_default[index]);
    n.partial = (s.number("fLockpickSkillPartialPickMult", 0) * skill + s.number("fLockpickSkillPartialPickBase", 1)) *
                s.number(k_partial[index], 26);
    n.break_seconds = (s.number("fLockpickBreakSkillMult", 0.005f) * skill + s.number("fLockpickBreakSkillBase", 1)) *
                      s.number(k_break[index], k_break_default[index]);
    return n;
}

void Game::reset_cell(uint32_t cell) {
    constexpr uint32_t k_persistent = 0x400, k_no_respawn = 0x40000000;
    constexpr int64_t k_cont_respawns = 0x02; // CONT DATA
    auto in_cell = [&](uint32_t ref) {
        const forms::FormEntry* e = m_db->find(ref);
        return e && e->parent == cell ? e : nullptr;
    };
    std::erase_if(m_containers, [&](const auto& c) {
        const forms::FormEntry* e = in_cell(c.first);
        const auto placed = e ? forms::placement(Form(*m_db, *e)) : std::nullopt;
        const forms::FormEntry* base = placed ? m_db->find(placed->base) : nullptr;
        if (!base) {
            return false;
        }
        const Value* flags = Form(*m_db, *base).get("DATA.flags");
        return flags && (flags->integer & k_cont_respawns) != 0;
    });
    std::erase_if(m_harvested, [&](uint32_t ref) { return in_cell(ref) != nullptr; });
    std::erase_if(m_taken, [&](uint32_t ref) {
        const forms::FormEntry* e = in_cell(ref);
        return e && !(e->flags & (k_persistent | k_no_respawn));
    });
}

bool Game::harvest(const Target& t) {
    if (t.kind != TargetKind::flora || m_harvested.contains(t.ref)) {
        return false;
    }
    const forms::FormEntry* base = m_db->find(t.base);
    const Form b(*m_db, *base);
    const uint32_t produce = form_of(b.get("PFIG"));
    const auto count = int32_t(std::floor(perk_value(87, 1.0f, {produce}) + 0.5f));
    std::vector<Rolled> rolled;
    m_leveled.roll(produce, count, uint16_t(m_character->level()), rolled);
    const std::string& added = m_settings.text("sAddItemtoInventory");
    uint32_t sound_form = form_of(b.get("SNAM"));
    for (const Rolled& r : rolled) {
        const Item* item = m_items.find(r.form);
        if (!item) {
            continue;
        }
        ItemExtra extra = r.extra;
        if (t.steal) {
            extra.owner = t.owner;
            m_crime.report_theft(t.ref, r.form, r.count, 0, t.owner, true);
        }
        m_player.inventory.add(r.form, r.count, extra);
        notify(r.count > 1 ? std::format("{} ({}) {}", item->name, r.count, added)
                           : std::format("{} {}", item->name, added));
    }
    if (const forms::FormEntry* s = m_db->find(sound_form)) {
        sound(std::string(m_db->editor_id(*s)));
    }
    m_harvested.insert(t.ref);
    return true;
}

Game::Vendor Game::vendor(uint32_t npc) const {
    Vendor v;
    const forms::FormEntry* e = m_db->find(npc);
    if (!e) {
        return v;
    }
    for (const forms::FactionMembership& m : forms::npc_factions(Form(*m_db, *e))) {
        const forms::FormEntry* fe = m_db->find(m.faction);
        if (m.rank < 0 || !fe) {
            continue;
        }
        const Form f(*m_db, *fe);
        const Value* values = f.get("VENV");
        if (!values || !f.get("VENC")) {
            continue;
        }
        v.faction = m.faction;
        v.chest = form_of(f.get("VENC"));
        v.list = form_of(f.get("VEND"));
        v.buys_stolen = integer(values->member("buys_stolen_items")) != 0;
        v.not_list = integer(values->member("not_buy_sell")) != 0;
        break;
    }
    return v;
}

void Game::refresh_vendor(const Vendor& v) {
    const auto it = m_vendor_stamps.find(v.chest);
    const double days = m_settings.number("iDaysToRespawnVendor", 2);
    if (it == m_vendor_stamps.end() || days_passed - it->second >= days) {
        m_vendor_stamps[v.chest] = days_passed;
        reset_container(v.chest);
    }
}

bool Game::vendor_accepts(const Vendor& v, const Item& item) const {
    if (item.kind == ItemKind::key) {
        return false;
    }
    if (!v.faction || !v.list) {
        return true;
    }
    if (item.keywords.empty()) {
        return false;
    }
    const forms::FormEntry* e = m_db->find(v.list);
    bool found = false;
    if (e) {
        for (const uint32_t k : forms::form_list(Form(*m_db, *e))) {
            found = found || item.has_keyword(k);
        }
    }
    return found != v.not_list;
}

float Game::buy_mult(uint32_t partner) const {
    const Settings& s = m_settings;
    const float skill = std::clamp(m_character->skill(speechcraft), 0.0f, 100.0f);
    const float v = skill / 100.0f * (s.number("fBarterMin", 2.0f) - s.number("fBarterMax", 3.3f)) +
                    s.number("fBarterMax", 3.3f);
    const float buy = perk_value(8, std::max(v, s.number("fBarterBuyMin", 1.05f)), {partner});
    return std::max({s.number("fMinBuyMult", 1.05f), buy, 1.0f});
}

float Game::sell_mult(uint32_t partner) const {
    const Settings& s = m_settings;
    const float skill = std::clamp(m_character->skill(speechcraft), 0.0f, 100.0f);
    const float v = skill / 100.0f * (s.number("fBarterMin", 2.0f) - s.number("fBarterMax", 3.3f)) +
                    s.number("fBarterMax", 3.3f);
    const float sell = perk_value(60, std::min(1.0f / v, s.number("fBarterSellMax", 0.95f)), {partner});
    return std::min({s.number("fMaxSellMult", 1.0f), sell, 1.0f});
}

bool Game::take(const Target& target) {
    if (target.kind != TargetKind::item || m_taken.contains(target.ref)) {
        return false;
    }
    ItemExtra extra;
    if (const auto it = m_placed.find(target.ref); it != m_placed.end()) {
        extra = it->second.stack.extra;
        m_placed.erase(it);
    } else {
        m_taken.insert(target.ref);
    }
    if (target.steal) {
        extra.owner = target.owner;
        m_crime.report_theft(target.ref, target.base, target.count, 0, target.owner, true);
    }
    sound(item_sound(*target.item, true, false));
    m_player.inventory.add(target.base, target.count, extra);
    return true;
}

void Game::update_worn_effects() {
    std::unordered_map<uint32_t, int> worn;
    std::unordered_map<uint32_t, uint32_t> pieces; // the armour each is on, the effect's source
    for (const Stack& s : m_player.inventory.stacks()) {
        const Item* item = m_items.find(s.form);
        if (!item || s.worn == Worn::none || item->kind != ItemKind::armor) {
            continue;
        }
        const uint32_t e = s.extra.enchantment ? s.extra.enchantment : item->enchantment;
        if (e && m_magic.enchantment_cast_type(e) == 0) {
            ++worn[e];
            pieces[e] = s.form;
        }
    }
    if (worn == m_worn_effects) {
        return;
    }
    for (const auto& [e, n] : m_worn_effects) {
        if (!worn.contains(e)) {
            m_character->dispel_spell(e);
        }
    }
    for (const auto& [e, n] : worn) {
        if (!m_worn_effects.contains(e)) {
            m_character->apply(e, k_player_ref, 0xFFFFFFFF, pieces[e]);
        }
    }
    m_worn_effects = std::move(worn);
}

float Game::damage_mult(int difficulty, bool to_player) const {
    static constexpr std::array<std::string_view, 6> to_pc = {"fDiffMultHPToPCVE", "fDiffMultHPToPCE", "fDiffMultHPToPCN",
                                                              "fDiffMultHPToPCH", "fDiffMultHPToPCVH", "fDiffMultHPToPCL"};
    static constexpr std::array<std::string_view, 6> by_pc = {"fDiffMultHPByPCVE", "fDiffMultHPByPCE", "fDiffMultHPByPCN",
                                                              "fDiffMultHPByPCH", "fDiffMultHPByPCVH", "fDiffMultHPByPCL"};
    static constexpr std::array<float, 6> to_defaults = {0.5f, 0.75f, 1, 1.5f, 2, 2.5f};
    static constexpr std::array<float, 6> by_defaults = {2, 1.5f, 1, 0.75f, 0.5f, 0.25f};
    const auto i = size_t(std::clamp(difficulty, 0, 5));
    return m_settings.number(to_player ? to_pc[i] : by_pc[i], to_player ? to_defaults[i] : by_defaults[i]);
}

bool Game::is_child(uint32_t npc) const {
    const forms::FormEntry* e = m_db->find(actor_record(*m_db, m_settings, npc, 0, 0).race);
    const Value* v = e ? Form(*m_db, *e).get("DATA.flags") : nullptr;
    return v && (v->integer & 0x4);
}

std::string Game::display_name_of(uint32_t npc) {
    constexpr uint16_t k_template_base_data = 0x80; // ACBS: the name is among what the template gives
    const forms::FormEntry* e = m_db->find(m_conditions.npc_trait(npc, k_template_base_data));
    return e ? std::string(Form(*m_db, *e).name()) : std::string();
}

bool Game::ignores_friendly_hits(uint32_t npc) const {
    return actor_record(*m_db, m_settings, npc, 0, 0).acbs_flags & 0x100000;
}

namespace {

int soul_level(int level, bool npc, const Settings& s) {
    if (npc || level >= s.number("iGrandSoulActorLevel", 38.0f)) {
        return 5;
    }
    if (level >= s.number("iGreaterSoulActorLevel", 28.0f)) {
        return 4;
    }
    if (level >= s.number("iCommonSoulActorLevel", 16.0f)) {
        return 3;
    }
    return level >= s.number("iLesserSoulActorLevel", 4.0f) ? 2 : 1;
}

} // namespace

float Game::soul_level_value(int level) const {
    static constexpr std::array<std::string_view, 5> names = {"iSoulLevelValuePetty", "iSoulLevelValueLesser", "iSoulLevelValueCommon",
                                                              "iSoulLevelValueGreater", "iSoulLevelValueGrand"};
    static constexpr std::array<float, 5> values = {250, 500, 1000, 2000, 3000};
    return level >= 1 && level <= 5 ? m_settings.number(names[size_t(level - 1)], values[size_t(level - 1)]) : 0.0f;
}

bool Game::has_npc_soul(uint32_t ref) const {
    // the ActorTypeNPC keyword, the NPC_'s or its race's.
    const uint32_t npc = base_of(ref);
    const uint32_t key = keyword("ActorTypeNPC");
    for (uint32_t form : {npc, actor_record(*m_db, m_settings, npc, 0, 0).race}) {
        if (const forms::FormEntry* e = m_db->find(form)) {
            const auto list = Form(*m_db, *e).keywords();
            if (std::find(list.begin(), list.end(), key) != list.end()) {
                return true;
            }
        }
    }
    return false;
}

float Game::soul_value_of(uint32_t npc) const {
    const int level = actor_record(*m_db, m_settings, npc, 0, 0).level;
    return soul_level_value(soul_level(level, has_npc_soul(npc), m_settings));
}

int Game::soul_level_of(uint32_t ref) {
    return magic_target(ref).soul_taken ? 0 : soul_level(actor_values(ref).record().level, has_npc_soul(ref), m_settings);
}

bool Game::is_actor(uint32_t ref) const {
    if (ref == k_player_ref) {
        return true;
    }
    const forms::FormEntry* e = m_db->find(ref);
    return e ? e->type == sig("ACHR") || e->type == sig("NPC_") : ref != 0 && !m_placed.contains(ref);
}

MagicTarget& Game::magic_target(uint32_t ref) {
    if (ref == k_player_ref) {
        return m_character->magic_target();
    }
    if (const auto it = m_magic_targets.find(ref); it != m_magic_targets.end()) {
        return *it->second;
    }
    MagicTarget& target = *m_magic_targets.emplace(ref, std::make_unique<MagicTarget>(*this, ref, actor_values(ref))).first->second;
    m_magic_order.push_back(&target);
    if (is_actor(ref)) {
        target.cast_permanent();
    }
    return target;
}

MagicTarget* Game::effect_target(uint32_t id) {
    const auto holds = [id](const MagicTarget& t) {
        return std::any_of(t.effects().begin(), t.effects().end(), [id](const ActiveEffect& e) { return e.id == id; });
    };
    if (holds(m_character->magic_target())) {
        return &m_character->magic_target();
    }
    for (MagicTarget* t : m_magic_order) {
        if (holds(*t)) {
            return t;
        }
    }
    return nullptr;
}

void Game::update_magic(float dt) {
    for (size_t i = 0; i < m_magic_order.size(); ++i) {
        m_magic_order[i]->update(dt);
    }
    if (dt > 0.0f) {
        m_casters.update(dt);
    }
}

void Game::notify_once(const std::string& text) {
    if (std::none_of(m_notices.begin(), m_notices.end(), [&](const Notice& n) { return n.text == text; })) {
        notify(text);
    }
}

bool Game::live_condition(uint16_t fn, uint32_t subject, uint32_t p1, uint32_t p2, double& out) {
    enum : uint16_t {
        GetLineOfSight = 27, GetDetected = 45, GetVelocity = 446, GetTalkedToPC = 50, GetHeadingAngle = 99, GetFriendHit = 288, GetShouldAttack = 66, IsRidingMount = 327, IsFleeing = 329,
        GetFactionRelation = 449, GetAllowWorldInteractions = 503, HasEquippedSpell = 570, GetCurrentCastingType = 571,
        GetCurrentDeliveryType = 572, GetRelativeAngle = 584, IsInScene = 590, GetIsFlying = 594, SpellHasKeyword = 596,
        IsFurnitureAnimType = 613, GetPathingTargetAngleOffset = 620, GetPathingTargetSpeed = 621, GetMovementSpeed = 623,
        GetFlyingState = 633, IsInFavorState = 635, GetBribeSuccess = 654, GetIntimidateSuccess = 655, GetPathingCurrentSpeed = 683,
        IsAllowedToFly = 698, IsCommandedActor = 700, IsRecoiling = 702, IsPathing = 704, IsHostileToActor = 719, GetTargetHeight = 720,
    };
    auto is = [&out](bool b) {
        out = b ? 1.0 : 0.0;
        return true;
    };
    const Agent* a = m_ai ? m_ai->agent(subject) : nullptr;
    const auto distance = [this, subject, p1] {
        const auto x = ref_position(subject), y = ref_position(p1);
        return x && y ? std::hypot((*x)[0] - (*y)[0], (*x)[1] - (*y)[1], (*x)[2] - (*y)[2]) : std::numeric_limits<float>::max();
    };
    const auto held_spell = [this, subject, p1]() -> const Spell* {
        return p1 <= source_voice ? m_magic.spell(m_casters.selected(subject, uint8_t(p1))) : nullptr;
    };
    const auto speech = [this](uint32_t ref) { return actor_values(ref).current(av_speechcraft); };
    switch (fn) {
    case GetLineOfSight:
        if (!p1) {
            return is(false);
        }
        return is(a && p1 == k_player_ref ? a->los : m_combat.world().line_of_sight(subject, p1));
    case GetDetected:
        if (!a || !p1) {
            return is(false);
        }
        return is(p1 == k_player_ref ? a->detection > 0 : m_combat.world().line_of_sight(subject, p1));
    case GetTalkedToPC: return is(ref_flag(subject ? subject : p1, ref_talked_to_pc));
    case GetShouldAttack: return is(a && p1 && m_combat_ai.should_attack(subject, a->npc, p1, base_of(p1), distance()));
    case IsHostileToActor: {
        const Fighter& f = m_combat.fighter(subject);
        return is(a && p1 && (f.combat_target == p1 || std::ranges::find(f.targets, p1) != f.targets.end() ||
                              m_combat_ai.should_attack(subject, a->npc, p1, base_of(p1), distance())));
    }
    case GetFactionRelation: out = a && p1 ? m_combat_ai.reaction(subject, a->npc, p1, base_of(p1)) : 0; return true;
    case IsFleeing: return is(m_combat_ai.fleeing(subject));
    case IsRecoiling: return is(m_combat.fighter(subject).recoiling);
    case IsCommandedActor: return is(magic_target(subject).commander != 0);
    case IsRidingMount: return is(subject == k_player_ref && m_player_riding);
    case GetFriendHit: out = double(m_combat.fighter(subject).friend_hits.size()); return true;
    case GetHeadingAngle: {
        out = 0;
        const auto to = p1 ? ref_position(p1) : std::nullopt, from = ref_position(subject);
        if (a && to && from) {
            const float direction = std::atan2((*to)[0] - (*from)[0], (*to)[1] - (*from)[1]);
            out = double(std::remainder(direction - a->heading, 6.2831853f)) * 57.29577791868205;
        }
        return true;
    }
    case IsInFavorState: return is(subject && favor_actor() == subject);
    case IsInScene: return is(m_quest_system && m_quest_system->scenes().has_actor(subject));
    // Bit 0x200 of the running package's interrupts; no package answers yes.
    case GetAllowWorldInteractions: return is(!a || !a->package || (a->package->interrupts & 0x200));
    case IsFurnitureAnimType: {
        if (subject == k_player_ref) { // the base's marker bits: sit, sleep, lean
            const forms::FormEntry* e = m_player_furniture ? m_db->find(base_of(m_player_furniture)) : nullptr;
            const int64_t bits = e ? integer(Form(*m_db, *e).get("MNAM")) : 0;
            return is((bits & 0x40000000 ? 1 : bits & 0x80000000 ? 2 : bits & 0x10000000 ? 4 : 0) == int(p1));
        }
        const bool in_it = a && a->use == UseState::using_it;
        const int type = !in_it ? 0 : a->activity == Activity::sleep ? 2 : a->activity == Activity::sit || a->activity == Activity::eat_sitting ? 1 : 0;
        return is(type == int(p1));
    }
    case HasEquippedSpell: {
        const Spell* s = held_spell();
        return is(s && s->type != spell_enchantment);
    }
    case GetCurrentCastingType:
        if (const Spell* s = held_spell()) {
            out = s->cast_type;
            return true;
        }
        return false;
    case GetCurrentDeliveryType:
        if (const Spell* s = held_spell()) {
            out = s->delivery;
            return true;
        }
        return false;
    case SpellHasKeyword: {
        const Spell* s = held_spell();
        return is(s && p2 && m_conditions.has_keyword(s->form, p2));
    }
    case 313: // GetPairedAnimation
        return is(combat().fighter(subject).in_kill_move || m_vats.pairing);
    case 391: // IsPC1stPerson
        return is(m_vats.active ? m_vats.first_person : m_world.player_first_person);
    case 407: { // GetVATSValue
        if (!m_vats.active) {
            out = 0;
            return true;
        }
        switch (p1) {
        case 4: out = m_vats.distance; return true;
        case 6: return is(int(p2) == m_vats.action);
        case 18: return is(p2 < 7 && (m_vats.projectile_flags >> (p2 + 16) & 1));
        case 19: return is(int(p2) == m_vats.delivery);
        case 20: return is(int(p2) == m_vats.casting);
        default: out = 0; return true;
        }
    }
    case 515: case 516: case 517: case 518: // GetVATSRight, Left, Back, FrontAreaFree
        out = m_vats.area_free ? m_vats.area_free(subject, int(fn) - 515) : 0.0;
        return true;
    case 522: case 523: case 524: case 525: // GetVATSRight, Left, Back, FrontTargetVisible
        out = m_vats.target_visible ? m_vats.target_visible(subject, p1, int(fn) - 522) : 0.0;
        return true;
    case GetRelativeAngle: {
        const uint32_t axis = p2 >= 'X' ? p2 - 'X' : p2; // the records hold the letter
        if (!p1 || axis > 2) {
            out = 0;
            return true;
        }
        constexpr float k_two_pi = 6.2831853f;
        float angle = ref_angle(subject)[axis] - ref_angle(p1)[axis];
        angle = std::fmod(std::fmod(angle, k_two_pi) + k_two_pi, k_two_pi);
        out = double(angle) * 57.295776;
        return true;
    }
    case GetTargetHeight: {
        const auto x = ref_position(subject), y = p1 ? ref_position(p1) : std::nullopt;
        out = x && y ? (*y)[2] - (*x)[2] : 0.0;
        return true;
    }
    case GetIsFlying: return is(a && a->fly != FlyState::none && a->fly != FlyState::perching);
    case GetFlyingState: out = a ? int(a->fly) : 0; return true;
    case IsAllowedToFly: return is(a && a->allow_flying);
    case IsPathing: return is(a && !a->path.empty());
    case GetVelocity: {
        // The character controller's vector, in units by axis letter; only the player's is kept.
        const size_t axis = p1 - 'X';
        out = subject == k_player_ref && axis < 3 ? m_combat.fighter(subject).velocity[axis] : 0.0;
        return true;
    }
    case GetMovementSpeed:
        if (subject == k_player_ref) {
            out = m_combat.fighter(subject).move_speed;
            return true;
        }
        [[fallthrough]];
    case GetPathingCurrentSpeed:
    case GetPathingTargetSpeed: out = a && !a->path.empty() ? a->speed : 0.0; return true;
    case GetPathingTargetAngleOffset: {
        out = 0;
        if (a && p1 == 2 && a->next < a->path.size()) { // the Z axis: how far the next point is off the heading
            const NavPoint& to = a->path[a->next];
            const float wanted = std::atan2(to[0] - a->position[0], to[1] - a->position[1]);
            out = double(std::remainder(wanted - a->heading, 6.2831853f)) * 57.295776;
        }
        return true;
    }
    case GetBribeSuccess: {
        if (!a) {
            return is(false);
        }
        const Settings& s = m_settings;
        const float level = float(m_character->level()), npc_level = float(actor_values(subject).record().level);
        const float base = level + npc_level * s.number("fBribeNPCLevelMult", 0.1f) -
                           (speech(k_player_ref) - speech(subject) - 100.0f) * s.number("fBribeSpeechcraftMult", 0.03f);
        float cost = std::pow(std::max(base, 0.0f), s.number("fBribeCostCurve", 1.5f)) * s.number("fBribeScale", 1.0f) *
                     (actor_values(subject).current(av_morality) * s.number("fBribeMoralityMult", 1.0f) + 1.0f);
        cost = perk_value(46, cost);
        return is(inventory_of(k_player_ref).count(k_gold) >= int(cost));
    }
    case GetIntimidateSuccess: {
        if (!a) {
            return is(false);
        }
        const Settings& s = m_settings;
        const float gap = std::max((speech(k_player_ref) - speech(subject)) * 0.01f, -1.0f) + 1.0f;
        const float power = perk_value(43, std::pow(gap, s.number("fIntimidateSpeechcraftCurve", 1.5f)) * float(m_character->level()));
        constexpr std::array<std::pair<const char*, float>, 5> k_confidence = {{{"fIntimidateConfidenceMultCowardly", 0.0f},
                                                                               {"fIntimidateConfidenceMultCautious", 0.5f},
                                                                               {"fIntimidateConfidenceMultAverage", 1.0f},
                                                                               {"fIntimidateConfidenceMultBrave", 1.5f},
                                                                               {"fIntimidateConfidenceMultFoolhardy", 100.0f}}};
        const auto& [name, fallback] = k_confidence[size_t(std::clamp(int(actor_values(subject).current(av_confidence)), 0, 4))];
        return is(power >= float(actor_values(subject).record().level) * s.number(name, fallback));
    }
    default: return false;
    }
}

uint32_t Game::place_form(uint32_t base, const std::array<float, 3>& position) {
    const uint32_t id = m_next_placed++;
    m_placed_forms[id] = base;
    m_moved[id] = position;
    return id;
}

void Game::inventory_changed(uint32_t owner, uint32_t form, int32_t delta) {
    if (!on_item_moved) {
        return;
    }
    if (delta < 0) {
        m_leaving.push_back({form, -delta, owner});
        return;
    }
    // Taken out of another inventory in the same step: it's a move.
    for (auto it = m_leaving.begin(); it != m_leaving.end() && delta > 0; ++it) {
        if (it->form == form && it->from != owner) {
            const int32_t n = std::min(it->count, delta);
            on_item_moved(form, n, it->from, owner);
            delta -= n;
            it->count -= n;
        }
    }
    std::erase_if(m_leaving, [](const Leaving& l) { return l.count <= 0; });
    if (delta > 0) {
        on_item_moved(form, delta, 0, owner);
    }
}

void Game::flush_item_moves() {
    for (const Leaving& l : std::exchange(m_leaving, {})) {
        if (on_item_moved) {
            on_item_moved(l.form, l.count, l.from, 0);
        }
    }
}

int32_t Game::count_of(const Inventory& inventory, uint32_t form) const {
    // GetItemCount sums a list's forms.
    const forms::FormEntry* e = m_db->find(form);
    if (!e || e->type != sig("FLST")) {
        return inventory.count(form);
    }
    int32_t n = 0;
    for (const uint32_t f : forms::form_list(Form(*m_db, *e))) {
        n += inventory.count(f);
    }
    return n;
}

void Game::read_enable_parents() const {
    if (m_read_parents) {
        return;
    }
    m_read_parents = true;
    for (const forms::Sig type : {sig("REFR"), sig("ACHR")}) {
        for (const forms::FormEntry* e : m_db->of_type(type)) {
            if (const auto f = m_db->field(*e, sig("XESP")); f && f->data.size() >= 5) {
                uint32_t parent = 0;
                std::memcpy(&parent, f->data.data(), 4);
                const bool opposite = (uint8_t(f->data[4]) & 1) != 0;
                m_enable_children[parent].push_back({e->form_id, opposite});
                m_enable_parent[e->form_id] = {parent, opposite};
            }
        }
    }
}

bool Game::ref_disabled(uint32_t ref) const {
    read_enable_parents();
    // A child is its parent's state, the opposite when it says so: its own record flag doesn't count
    if (const auto parent = m_enable_parent.find(ref); parent != m_enable_parent.end()) {
        return ref_disabled(parent->second.ref) != parent->second.opposite;
    }
    if (const auto it = m_own_disabled.find(ref); it != m_own_disabled.end()) {
        return it->second;
    }
    const forms::FormEntry* e = m_db->find(ref);
    return e && (e->flags & 0x800) != 0; // the record's Initially Disabled
}

void Game::publish_ref_state(uint32_t ref) {
    const bool disabled = ref_disabled(ref);
    m_ref_overrides[ref] = disabled;
    if (on_ref_state) {
        on_ref_state(ref, disabled);
    }
    if (const auto it = m_enable_children.find(ref); it != m_enable_children.end()) {
        for (const EnableChild& c : it->second) {
            publish_ref_state(c.ref);
        }
    }
}

bool Game::set_ref_disabled(uint32_t ref, bool disabled) {
    read_enable_parents();
    if (m_enable_parent.contains(ref)) {
        return false; // "cannot enable/disable an object with an enable state parent"
    }
    m_own_disabled[ref] = disabled;
    publish_ref_state(ref);
    return true;
}

void Game::block_activation(uint32_t ref, bool blocked) {
    if (blocked) {
        m_blocked.insert(ref);
    } else {
        m_blocked.erase(ref);
    }
}

bool Game::survival() const {
    static const uint32_t toggle = [this] {
        for (const forms::FormEntry* e : m_db->of_type(sig("GLOB"))) {
            if (m_db->editor_id(*e) == "Survival_ModeToggle") {
                return e->form_id;
            }
        }
        return 0u;
    }();
    float value = 0;
    return toggle && script_facts && script_facts->global_value(toggle, value) && value == 1.0f;
}

int32_t Game::stat(const std::string& name) const {
    if (name == "Days Jailed") {
        return m_crime.stats().days_jailed;
    }
    const auto it = m_stats.find(name);
    return it == m_stats.end() ? 0 : it->second;
}

void Game::increment_stat(const std::string& name, int32_t by) { m_stats[name] += by; }

float Game::warmth() const {
    float total = m_character->values().current(76); // Variable09
    for (const Stack& stack : m_player.inventory.stacks()) {
        const Item* item = m_items.find(stack.form);
        if (item && stack.worn != Worn::none) {
            total += item_warmth(*item);
        }
    }
    return total;
}

float Game::slot_warmth(const Item& item) const {
    // As slot_rating: what the pieces worn in the item's slots give, each once.
    float total = 0;
    for (const Stack& s : m_player.inventory.stacks()) {
        const Item* worn = s.worn != Worn::none ? m_items.find(s.form) : nullptr;
        if (worn && worn->kind == ItemKind::armor && (worn->biped_slots & item.biped_slots)) {
            total += item_warmth(*worn);
        }
    }
    return total;
}

float Game::item_warmth(const Item& piece) const {
    // WarmthCalcFunc: a bonus per slot the piece's keywords name, the Warm or Cold set when it has those keywords.
    const Settings& s = m_settings;
    const uint32_t warm = keyword("Survival_ArmorWarm"), cold = keyword("Survival_ArmorCold"), jewelry = keyword("ArmorJewelry");
    struct Slot {
        const char *armor, *clothing, *normal, *hot, *chilly;
        float normal_value, hot_value, chilly_value;
    };
    static constexpr Slot k_slots[4] = {
        {"ArmorCuirass", "ClothingBody", "fSurvNormalBodyBonus", "fSurvWarmBodyBonus", "fSurvColdBodyBonus", 27, 54, 17},
        {"ArmorHelmet", "ClothingHead", "fSurvNormalHeadBonus", "fSurvWarmHeadBonus", "fSurvColdHeadBonus", 18, 29, 8},
        {"ArmorGauntlets", "ClothingHands", "fSurvNormalHandsBonus", "fSurvWarmHandsBonus", "fSurvColdHandsBonus", 13, 24, 7},
        {"ArmorBoots", "ClothingFeet", "fSurvNormalFeetBonus", "fSurvWarmFeetBonus", "fSurvColdFeetBonus", 13, 24, 7},
    };
    float total = 0;
    {
        const Item* item = &piece;
        const auto has = [item](uint32_t k) { return k && std::find(item->keywords.begin(), item->keywords.end(), k) != item->keywords.end(); };
        if (item->kind == ItemKind::light) {
            total += s.number("fSurvTorchBonus", 50);
        }
        if (item->kind != ItemKind::armor || has(jewelry)) {
            return total;
        }
        for (const Slot& slot : k_slots) {
            if (has(keyword(slot.armor)) || has(keyword(slot.clothing))) {
                const float bonus = has(warm)   ? s.number(slot.hot, slot.hot_value)
                                    : has(cold) ? s.number(slot.chilly, slot.chilly_value)
                                                : s.number(slot.normal, slot.normal_value);
                total += bonus * s.number("fSurvArmorScalar", 1);
            }
        }
    }
    return total;
}

bool Game::in_region(uint32_t region) const {
    // The region's areas (RPLD) around the player, in world units.
    const forms::FormEntry* e = m_db->find(region);
    if (!e) {
        return false;
    }
    const float x = m_world.player_position[0], y = m_world.player_position[1];
    for (const forms::Field& f : m_db->fields(*e)) {
        if (f.sig != sig("RPLD")) {
            continue;
        }
        const size_t count = f.data.size() / 8;
        std::vector<std::array<float, 2>> p(count);
        std::memcpy(p.data(), f.data.data(), count * 8);
        bool inside = false;
        for (size_t i = 0, j = count - 1; i < count; j = i++) {
            if ((p[i][1] > y) != (p[j][1] > y) && x < (p[j][0] - p[i][0]) * (y - p[i][1]) / (p[j][1] - p[i][1]) + p[i][0]) {
                inside = !inside;
            }
        }
        if (inside) {
            return true;
        }
    }
    return false;
}

std::optional<std::array<float, 3>> Game::ref_position(uint32_t ref) const {
    if (ref == k_player_ref) {
        return m_world.player_position;
    }
    if (const Agent* a = m_ai ? m_ai->agent(ref) : nullptr) {
        return std::array<float, 3>{a->position[0], a->position[1], a->position[2]}; // where it is, not where it was placed
    }
    if (const auto it = m_moved.find(ref); it != m_moved.end()) {
        return it->second;
    }
    if (const forms::FormEntry* e = m_db->find(ref)) {
        if (const auto placed = forms::placement(Form(*m_db, *e))) {
            return placed->position;
        }
    }
    return std::nullopt;
}

std::string Game::body_part_node(uint32_t ref) {
    const uint32_t npc = ref == k_player_ref ? 7u : m_ai ? (m_ai->agent(ref) ? m_ai->agent(ref)->npc : 0u) : 0u;
    const forms::FormEntry* n = m_db->find(npc);
    const Value* race = n ? Form(*m_db, *n).get("RNAM") : nullptr;
    const forms::FormEntry* r = race ? m_db->find(race->form) : nullptr;
    const Value* data = r ? Form(*m_db, *r).get("GNAM") : nullptr;
    const forms::FormEntry* b = data ? m_db->find(data->form) : nullptr;
    if (!b) {
        return {};
    }
    std::string node;
    for (const auto& field : Form(*m_db, *b).fields()) {
        if (field.sig == forms::sig("BPNT")) {
            node = field.value.text;
        } else if (field.sig == forms::sig("BPND")) {
            if (const Value* type = field.value.member("part_type"); type && type->integer == 0) {
                std::ranges::transform(node, node.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
                return node;
            }
        }
    }
    return {};
}

std::array<float, 3> Game::ref_angle(uint32_t ref) const {
    if (ref == Conditions::k_player) {
        return {0.0f, 0.0f, m_world.player_heading};
    }
    if (const Agent* a = m_ai ? m_ai->agent(ref) : nullptr) {
        return {0.0f, 0.0f, a->heading}; // where it has turned to since
    }
    if (const RefChanges* c = find_changes(ref); c && c->angle) {
        return *c->angle;
    }
    if (const forms::FormEntry* e = m_db->find(ref)) {
        if (const auto placed = forms::placement(Form(*m_db, *e))) {
            return placed->rotation;
        }
    }
    return {};
}

float Game::ref_scale(uint32_t ref) const {
    float scale = 1.0f;
    const forms::FormEntry* e = m_db->find(ref);
    const auto placed = e ? forms::placement(Form(*m_db, *e)) : std::nullopt;
    if (placed) {
        scale = placed->scale;
    }
    if (const RefChanges* c = find_changes(ref); c && c->scale) {
        scale = *c->scale;
    }
    // An actor's counts its NPC_'s height.
    if (const forms::FormEntry* b = placed ? m_db->find(placed->base) : nullptr; b && b->type == sig("NPC_")) {
        if (const Value* height = Form(*m_db, *b).get("NAM6")) {
            scale *= float(height->real);
        }
    }
    return scale;
}

ActorValues& Game::actor_values(uint32_t ref) {
    if (ref == k_player_ref) {
        return m_character->values();
    }
    if (const auto it = m_actor_values.find(ref); it != m_actor_values.end()) {
        return it->second;
    }
    uint32_t npc = ref;
    if (const forms::FormEntry* e = m_db->find(ref); e && e->type != sig("NPC_")) {
        npc = form_of(Form(*m_db, *e).get("NAME"));
    } else if (!e) {
        const Agent* placed = m_ai ? m_ai->agent(ref) : nullptr; // made at run time: no record, its agent knows the base
        npc = placed ? placed->npc : ref;
    }
    if (const forms::FormEntry* e = m_db->find(npc); e && e->type == sig("LVLN")) {
        std::vector<Rolled> rolled; // a leveled actor's values are those of what it came to
        m_leveled.roll(npc, 1, 1, rolled);
        npc = rolled.empty() ? npc : rolled.front().form;
    }
    ActorValues values(*m_db, m_settings, false);
    values.set_record(actor_record(*m_db, m_settings, npc, 0, 0));
    return m_actor_values.emplace(ref, std::move(values)).first->second;
}

bool Game::recipe_passes(uint32_t record, uint32_t item, bool enchanted) {
    m_tempering_item = item;
    m_tempering_enchanted = enchanted;
    const forms::FormEntry* e = m_db->find(record);
    const bool passes = e && m_conditions.evaluate(condition_list(Form(*m_db, *e)), k_player_ref, 0);
    m_tempering_item = 0;
    m_tempering_enchanted = false;
    return passes;
}

bool Game::condition_value(uint16_t fn, uint32_t subject, uint32_t target, uint32_t p1, uint32_t p2, double& out) {
    // Condition functions over the player's items.
    constexpr uint16_t k_get_pos = 6, k_is_moving = 25, k_get_disabled = 35, k_is_player_in_region = 117, k_is_swimming = 185,
                       k_spell_has_keyword = 693, k_spell_has_skill = 696, k_spell_has_casting_perk = 713, k_actor_warmth = 735;
    constexpr uint16_t k_get_random_percent = 77, k_get_item_count = 47, k_get_equipped = 182, k_get_equipped_item_type = 597,
                       k_tempering_enchanted = 659, k_tempering_keyword = 660, k_worn_has_keyword = 682,
                       k_worn_keyword_count = 722, k_is_over_encumbered = 734;
    auto is = [&out](bool b) {
        out = b ? 1.0 : 0.0;
        return true;
    };
    // What scripts and the AI have changed since the records by function.
    enum : uint16_t {
        GetAngle = 8, GetScale = 24, GetDead = 46, GetGold = 48, GetSleeping = 49, GetLocked = 5, GetLockLevel = 65,
        GetInFaction = 71, GetFactionRank = 73, GetDeadCount = 84, GetAlarmed = 61, GetIsAlerted = 91, IsGuard = 125, IsTrespassing = 145,
        GetSitting = 159, GetIsCurrentPackage = 161, GetIsGhost = 237, GetRestrained = 244, IsInDialogueWithPlayer = 249,
        IsSneaking = 286, IsEssential = 354, GetRelationshipRank = 403, GetFactionCombatReaction = 410,
        GetPlayerTeammate = 453, GetPlayerTeammateCount = 454, GetCrimeGold = 459, HasLoaded3D = 555, HasRefType = 561,
        GetKeywordDataForLocation = 606, GetKeywordDataForCurrentLocation = 651, GetEquippedShout = 579, IsUndead = 715,
    };
    if (live_condition(fn, subject, p1, p2, out)) {
        return true;
    }
    switch (fn) {
    case GetAngle: {
        const uint32_t axis = p1 >= 'X' ? p1 - 'X' : p1;
        if (axis > 2) {
            out = 0.0;
            return true;
        }
        out = double(ref_angle(subject)[axis]) * 57.295776;
        return true;
    }
    case GetScale: out = ref_scale(subject); return true;
    case GetDead: return is(subject != k_player_ref && ref_dead && ref_dead(subject));
    case GetGold: out = inventory_of(subject).count(k_gold); return true;
    case GetSleeping: out = sleep_state(subject); return true;
    case GetSitting: out = sit_state(subject); return true;
    case GetInFaction: return is(faction_rank(subject, p1) >= 0);
    case GetFactionRank: out = faction_rank(subject, p1); return true;
    case GetDeadCount: out = dead_count(p1); return true;
    case GetIsAlerted: return is(ref_flag(subject, ref_alert));
    case IsGuard: return is(m_crime.is_guard(base_of(subject)));
    case IsTrespassing: return is(subject == k_player_ref && m_crime.trespassing());
    case GetIsCurrentPackage: {
        const Agent* a = ai().agent(subject);
        return is(a && a->package && a->package->form == p1);
    }
    case GetIsGhost: return is(is_ghost(subject));
    case GetRestrained: return is(ref_flag(subject, ref_restrained));
    case IsInDialogueWithPlayer: return is(dialogue_speaker && dialogue_speaker() == subject && subject != 0);
    case IsSneaking: return is(subject == k_player_ref && m_world.player_sneaking);
    case IsEssential: return is(base_flag(base_of(subject), 0x2));
    case GetRelationshipRank: out = m_crime.relationship(base_of(subject), base_of(p1)); return true;
    case GetFactionCombatReaction: out = p1 && p2 ? m_crime.faction_reaction(p1, p2) : 0; return true;
    case GetPlayerTeammate: return is(ref_flag(subject, ref_teammate));
    case GetPlayerTeammateCount: {
        out = double(std::count_if(m_changes.begin(), m_changes.end(), [](const auto& e) { return (e.second.flags & ref_teammate) != 0; }));
        return true;
    }
    case GetCrimeGold: out = is_actor(subject) ? m_crime.crime_gold(crime_faction(subject)) : 0.0; return true; // the subject's faction
    case GetAlarmed: { // the current package is Alarm: a guard coming for the player
        const Agent* a = m_ai ? m_ai->agent(subject) : nullptr;
        return is(a && a->arresting);
    }
    case HasLoaded3D: return is(ref_loaded && ref_loaded(subject));
    case HasRefType: {
        const forms::FormEntry* e = p1 ? m_db->find(subject) : nullptr;
        if (!e) {
            return is(false);
        }
        const Form f(*m_db, *e);
        for (const Value* v : f.all("XLRT")) {
            if (form_of(v) == p1) {
                return is(true);
            }
        }
        return is(false);
    }
    case GetEquippedShout: {
        const Shout* shout = m_casters.selected_shout(subject);
        return is((shout ? shout->form : 0) == p1);
    }
    case IsUndead: {
        // Default object 156, "Keyword - Undead".
        constexpr uint32_t k_undead = 0x00013796;
        const forms::FormEntry* e = m_db->find(base_of(subject));
        const auto own = e ? Form(*m_db, *e).keywords() : std::vector<uint32_t>{};
        const forms::FormEntry* race = m_db->find(actor_values(subject).record().race);
        const auto of_race = race ? Form(*m_db, *race).keywords() : std::vector<uint32_t>{};
        return is(std::ranges::find(own, k_undead) != own.end() || std::ranges::find(of_race, k_undead) != of_race.end());
    }
    case GetKeywordDataForLocation: out = location_data(p1, p2); return true;
    case GetKeywordDataForCurrentLocation: out = location_data(m_world.player_location, p1); return true;
    default: break;
    }
    if (m_combat.condition_value(fn, subject, target, p1, p2, out)) {
        return true;
    }
    if (fn == k_get_random_percent) {
        out = std::floor(m_character->events.random_percent());
        return true;
    }
    if (magic_condition(fn, subject, p1, out)) {
        return true;
    }
    if (fn == k_get_disabled) {
        return is(ref_disabled(subject));
    }
    if (fn == k_is_player_in_region) {
        return is(in_region(p1));
    }
    // A perk's spell tab: the game's scratch reference holds the spell; here the subject is the spell.
    if (fn == k_spell_has_keyword || fn == k_spell_has_skill || fn == k_spell_has_casting_perk) {
        const forms::FormEntry* e = m_db->find(subject);
        if (!e) {
            return is(false);
        }
        const Form f(*m_db, *e);
        if (fn == k_spell_has_keyword) {
            for (const auto& effect : forms::effects(f)) {
                const MagicEffect* m = m_magic.effect(effect.effect);
                if (m && std::find(m->keywords.begin(), m->keywords.end(), p1) != m->keywords.end()) {
                    return is(true);
                }
            }
            const auto own = f.keywords();
            return is(std::find(own.begin(), own.end(), p1) != own.end());
        }
        if (fn == k_spell_has_casting_perk) {
            const Value* perk = f.get("SPIT.half_cost_perk");
            return is(perk && perk->form == p1);
        }
        // The costliest effect's skill.
        const MagicEffect* costliest = nullptr;
        float cost = -1;
        for (const auto& effect : forms::effects(f)) {
            const MagicEffect* m = m_magic.effect(effect.effect);
            const float c = m ? m_magic.effect_cost(Effect{effect.effect, effect.magnitude, effect.area, effect.duration}) : -1;
            if (m && c > cost) {
                cost = c;
                costliest = m;
            }
        }
        return is(costliest && costliest->magic_skill == int32_t(p1));
    }
    if (fn == k_tempering_enchanted) {
        return is(m_tempering_enchanted);
    }
    if (fn == k_tempering_keyword) {
        const Item* item = m_items.find(m_tempering_item);
        return is(item && std::find(item->keywords.begin(), item->keywords.end(), p1) != item->keywords.end());
    }
    if (subject == k_player_ref) {
        switch (fn) {
        case k_get_item_count: out = count_of(m_player.inventory, p1); return true;
        case k_get_equipped:
            return is(std::any_of(m_player.inventory.stacks().begin(), m_player.inventory.stacks().end(),
                                  [p1](const Stack& s) { return s.form == p1 && s.worn != Worn::none; }));
        case k_worn_has_keyword: case k_worn_keyword_count: {
            int count = 0;
            for (const Stack& s : m_player.inventory.stacks()) {
                const Item* item = m_items.find(s.form);
                if (item && item->kind == ItemKind::armor && s.worn != Worn::none &&
                    std::find(item->keywords.begin(), item->keywords.end(), p1) != item->keywords.end()) {
                    ++count;
                }
            }
            out = fn == k_worn_has_keyword ? (count ? 1.0 : 0.0) : double(count);
            return true;
        }
        case k_is_over_encumbered: return is(over_encumbered());
        case k_get_equipped_item_type: out = equipped_item_type(p1 == 0); return true; // 0 left, 1 right
        case k_get_pos: out = m_world.player_position[std::min<size_t>(p1 >= 'X' ? p1 - 'X' : p1, 2)]; return true;
        case k_is_moving: return is(m_world.player_moving);
        case k_is_swimming: return is(m_world.player_swimming);
        case k_actor_warmth: out = warmth(); return true;
        default: break;
        }
    }
    return m_character->condition_value(fn, subject, target, p1, p2, out) ||
           (script_facts && script_facts->condition_value(fn, subject, target, p1, p2, out));
}

bool Game::magic_condition(uint16_t fn, uint32_t subject, uint32_t p1, double& out) {
    // Functions on a magic item: the subject is the spell an entry point was handed.
    constexpr uint16_t k_get_disease = 39, k_has_magic_effect = 214, k_is_spell_target = 223, k_get_is_ghost = 237,
                       k_has_spell = 264, k_is_in_combat = 289, k_is_dual_casting = 627, k_spell_has_keyword = 693,
                       k_spell_has_skill = 696, k_has_effect_keyword = 699, k_is_commanded = 700,
                       k_spell_has_casting_perk = 713, k_effect_was_dual_cast = 724;
    const auto is = [&out](bool b) {
        out = b ? 1.0 : 0.0;
        return true;
    };
    switch (fn) {
    case k_spell_has_keyword: {
        const forms::FormEntry* e = m_db->find(subject);
        const auto list = e ? Form(*m_db, *e).keywords() : std::vector<uint32_t>{};
        return is(std::find(list.begin(), list.end(), p1) != list.end());
    }
    case k_spell_has_skill: {
        const Spell* s = m_magic.spell(subject);
        const SpellEffect* top = s ? m_magic.costliest(*s) : nullptr;
        const MagicEffect* m = top ? m_magic.effect(top->effect) : nullptr;
        return is(m && m->magic_skill == int(p1));
    }
    case k_spell_has_casting_perk: {
        const Spell* s = m_magic.spell(subject);
        return is(s && s->half_cost_perk == p1);
    }
    case k_is_dual_casting: return is(m_casters.dual_casting(subject));
    case k_effect_was_dual_cast: return is(m_casters.dual_flagged == subject);
    default: break;
    }
    if ((fn != k_get_disease && fn != k_has_magic_effect && fn != k_is_spell_target && fn != k_get_is_ghost && fn != k_has_spell &&
         fn != k_is_in_combat && fn != k_has_effect_keyword && fn != k_is_commanded) || !is_actor(subject)) {
        return false;
    }
    MagicTarget& target = magic_target(subject);
    switch (fn) {
    case k_get_disease: return is(target.has_disease());
    case k_has_magic_effect: return is(target.has_magic_effect(p1));
    case k_is_spell_target: return is(target.has_effect_from(p1));
    case k_get_is_ghost: return is(target.ghost());
    case k_has_spell: return is(target.has_spell(p1));
    case k_is_in_combat: return is(m_combat.fighter(subject).in_combat);
    case k_has_effect_keyword: return is(target.has_magic_effect_keyword(p1));
    default: return is(target.commander != 0);
    }
}

bool Game::global_value(uint32_t global, float& out) { return script_facts && script_facts->global_value(global, out); }

uint32_t Game::furniture_active_markers(uint32_t furniture) const {
    const forms::FormEntry* e = m_db->find(furniture);
    const forms::Value* v = e ? forms::Form(*m_db, *e).get("MNAM") : nullptr;
    return v ? uint32_t(v->integer) & 0xFFFFFF : 0;
}

uint16_t Game::furniture_entry_points(uint32_t furniture, uint32_t marker) const {
    const forms::FormEntry* e = m_db->find(furniture);
    if (!e) {
        return 0;
    }
    const forms::Form f(*m_db, *e);
    // ENAM, NAM0, FNMK and FNPR repeat per marker; a record with one FNPR and no ENAM has it for marker 0.
    const auto points = f.all("FNPR");
    const auto numbers = f.all("ENAM");
    for (size_t i = 0; i < points.size(); ++i) {
        const uint32_t number = i < numbers.size() ? uint32_t(numbers[i]->integer) : uint32_t(i);
        const forms::Value* entry = points[i]->path("entry_points");
        if (number == marker && entry) {
            return uint16_t(entry->integer);
        }
    }
    return 0;
}

} // namespace spud::game
