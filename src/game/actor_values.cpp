#include "game/actor_values.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace spud::game {

namespace {

// names, types and flags.
constexpr ActorValueInfo k_values[k_actor_value_count] = {
    {"Aggression", 2, 0x44100}, {"Confidence", 2, 0x4100}, {"Energy", 2, 0x0}, {"Morality", 2, 0x44100},
    {"Mood", 2, 0x4100}, {"Assistance", 2, 0x44100}, {"OneHanded", 1, 0xC0010}, {"TwoHanded", 1, 0xC0010},
    {"Marksman", 1, 0xC0010}, {"Block", 1, 0xC0010}, {"Smithing", 1, 0xC0010}, {"HeavyArmor", 1, 0xC0010},
    {"LightArmor", 1, 0xC0010}, {"Pickpocket", 1, 0xC0010}, {"Lockpicking", 1, 0xC0010}, {"Sneak", 1, 0xC0010},
    {"Alchemy", 1, 0xC0010}, {"Speechcraft", 1, 0xC0010}, {"Alteration", 1, 0xC0012}, {"Conjuration", 1, 0xC0012},
    {"Destruction", 1, 0xC0012}, {"Illusion", 1, 0xC0012}, {"Restoration", 1, 0xC0012}, {"Enchanting", 1, 0xC0010},
    {"Health", 0, 0xC0000}, {"Magicka", 0, 0x1C0020}, {"Stamina", 0, 0xC0020}, {"HealRate", 0, 0x40800},
    {"MagickaRate", 0, 0x40800}, {"StaminaRate", 0, 0x40800}, {"SpeedMult", 0, 0x40800},
    {"InventoryWeight", 0, 0x0}, {"CarryWeight", 0, 0x40800}, {"CritChance", 0, 0x800}, {"MeleeDamage", 0, 0x800},
    {"UnarmedDamage", 0, 0x800}, {"Mass", 0, 0x800}, {"VoicePoints", 0, 0xC0000}, {"VoiceRate", 0, 0x40800},
    {"DamageResist", 3, 0x800}, {"PoisonResist", 3, 0x8000}, {"FireResist", 3, 0x8000},
    {"ElectricResist", 3, 0x8000}, {"FrostResist", 3, 0x8000}, {"MagicResist", 3, 0x8000},
    {"DiseaseResist", 3, 0x8000}, {"PerceptionCondition", 4, 0x22820}, {"EnduranceCondition", 4, 0x22820},
    {"LeftAttackCondition", 4, 0x22820}, {"RightAttackCondition", 4, 0x22820},
    {"LeftMobilityCondition", 4, 0x22820}, {"RightMobilityCondition", 4, 0x22820}, {"BrainCondition", 4, 0x22820},
    {"Paralysis", 5, 0x48000}, {"Invisibility", 5, 0x48000}, {"NightEye", 5, 0x8000},
    {"DetectLifeRange", 5, 0x8000}, {"WaterBreathing", 5, 0x8000}, {"WaterWalking", 5, 0x8000},
    {"IgnoreCrippledLimbs", 6, 0xA000}, {"Fame", 6, 0x8000}, {"Infamy", 6, 0x8000}, {"JumpingBonus", 6, 0x8000},
    {"WardPower", 6, 0x48000}, {"RightItemCharge", 6, 0x140000}, {"ArmorPerks", 6, 0x8000},
    {"ShieldPerks", 6, 0x8000}, {"WardDeflection", 6, 0x48000}, {"Variable01", 6, 0x0}, {"Variable02", 6, 0x0},
    {"Variable03", 6, 0x0}, {"Variable04", 6, 0x0}, {"Variable05", 6, 0x0}, {"Variable06", 6, 0x0},
    {"Variable07", 6, 0x0}, {"Variable08", 6, 0x0}, {"Variable09", 6, 0x0}, {"Variable10", 6, 0x0},
    {"BowSpeedBonus", 6, 0x800}, {"FavorActive", 6, 0x8000}, {"FavorsPerDay", 6, 0x8000},
    {"FavorsPerDayTimer", 6, 0x8000}, {"LeftItemCharge", 6, 0x140000}, {"AbsorbChance", 6, 0x48000},
    {"Blindness", 6, 0x8000}, {"WeaponSpeedMult", 6, 0x48000}, {"ShoutRecoveryMult", 6, 0x10200},
    {"BowStaggerBonus", 6, 0x88000}, {"Telekinesis", 6, 0x8000}, {"FavorPointsBonus", 6, 0x8000},
    {"LastBribedIntimidated", 6, 0x8000}, {"LastFlattered", 6, 0x8000}, {"MovementNoiseMult", 6, 0x10200},
    {"BypassVendorStolenCheck", 6, 0x8000}, {"BypassVendorKeywordCheck", 6, 0x8000},
    {"WaitingForPlayer", 6, 0x8000}, {"OneHandedMod", 6, 0x8000}, {"TwoHandedMod", 6, 0x8000},
    {"MarksmanMod", 6, 0x8000}, {"BlockMod", 6, 0x8000}, {"SmithingMod", 6, 0x8000}, {"HeavyArmorMod", 6, 0x8000},
    {"LightArmorMod", 6, 0x8000}, {"PickPocketMod", 6, 0x8000}, {"LockpickingMod", 6, 0x8000},
    {"SneakMod", 6, 0x8000}, {"AlchemyMod", 6, 0x8000}, {"SpeechcraftMod", 6, 0x8000},
    {"AlterationMod", 6, 0x8000}, {"ConjurationMod", 6, 0x8000}, {"DestructionMod", 6, 0x8000},
    {"IllusionMod", 6, 0x8000}, {"RestorationMod", 6, 0x8000}, {"EnchantingMod", 6, 0x8000},
    {"OneHandedSkillAdvance", 6, 0x8000}, {"TwoHandedSkillAdvance", 6, 0x8000},
    {"MarksmanSkillAdvance", 6, 0x8000}, {"BlockSkillAdvance", 6, 0x8000}, {"SmithingSkillAdvance", 6, 0x8000},
    {"HeavyArmorSkillAdvance", 6, 0x8000}, {"LightArmorSkillAdvance", 6, 0x8000},
    {"PickPocketSkillAdvance", 6, 0x8000}, {"LockpickingSkillAdvance", 6, 0x8000},
    {"SneakSkillAdvance", 6, 0x8000}, {"AlchemySkillAdvance", 6, 0x8000}, {"SpeechcraftSkillAdvance", 6, 0x8000},
    {"AlterationSkillAdvance", 6, 0x8000}, {"ConjurationSkillAdvance", 6, 0x8000},
    {"DestructionSkillAdvance", 6, 0x8000}, {"IllusionSkillAdvance", 6, 0x8000},
    {"RestorationSkillAdvance", 6, 0x8000}, {"EnchantingSkillAdvance", 6, 0x8000},
    {"LeftWeaponSpeedMult", 6, 0x48000}, {"DragonSouls", 6, 0x8000}, {"CombatHealthRegenMult", 6, 0x248000},
    {"OneHandedPowerMod", 6, 0x8000}, {"TwoHandedPowerMod", 6, 0x8000}, {"MarksmanPowerMod", 6, 0x8000},
    {"BlockPowerMod", 6, 0x8000}, {"SmithingPowerMod", 6, 0x8000}, {"HeavyArmorPowerMod", 6, 0x8000},
    {"LightArmorPowerMod", 6, 0x8000}, {"PickPocketPowerMod", 6, 0x8000}, {"LockpickingPowerMod", 6, 0x8000},
    {"SneakPowerMod", 6, 0x8000}, {"AlchemyPowerMod", 6, 0x8000}, {"SpeechcraftPowerMod", 6, 0x8000},
    {"AlterationPowerMod", 6, 0x8000}, {"ConjurationPowerMod", 6, 0x8000}, {"DestructionPowerMod", 6, 0x8000},
    {"IllusionPowerMod", 6, 0x8000}, {"RestorationPowerMod", 6, 0x8000}, {"EnchantingPowerMod", 6, 0x8000},
    {"DragonRend", 6, 0x8000}, {"AttackDamageMult", 6, 0x50000}, {"HealRateMult", 6, 0x60000},
    {"MagickaRateMult", 6, 0x60000}, {"StaminaRateMult", 6, 0x60000}, {"WerewolfPerks", 1, 0x8000},
    {"VampirePerks", 1, 0x8000}, {"GrabActorOffset", 6, 0x8000}, {"Grabbed", 6, 0x8000},
    {"DEPRECATED05", 6, 0x2000}, {"ReflectDamage", 6, 0x8000},
};

constexpr float k_essential_minimum_health = -30.0f; // fEssentialNPCMinimumHealth, compiled

using forms::Value;

int64_t member_int(const Value* v, std::string_view path) {
    const Value* m = v ? v->path(path) : nullptr;
    return m ? m->integer : 0;
}

double member_real(const Value* v, std::string_view path) {
    const Value* m = v ? v->path(path) : nullptr;
    return m ? (m->kind == Value::Kind::real ? m->real : double(m->integer)) : 0.0;
}

struct AutoEntry {
    float* value;
    int weight;
    float cap; // 0: none
};

// shares by weight, the highest first (the later value first on equal weights),
// what capped values can't take goes to the others, then what's left point by point.
void distribute(std::vector<AutoEntry> entries, int pool) {
    int total = 0;
    for (const AutoEntry& e : entries) {
        total += e.weight;
    }
    if (pool <= 0 || total <= 0) {
        return;
    }
    std::reverse(entries.begin(), entries.end());
    std::stable_sort(entries.begin(), entries.end(), [](const AutoEntry& a, const AutoEntry& b) { return a.weight > b.weight; });
    const int share = pool / total;
    const int left = pool - total * share;
    int excess = 0;
    auto give = [&](AutoEntry& e, int points) {
        *e.value += float(points);
        if (e.cap > 0.0f && *e.value > e.cap) {
            excess += int(*e.value - e.cap);
            *e.value = e.cap;
        }
    };
    for (AutoEntry& e : entries) {
        give(e, share * e.weight);
    }
    auto open = [](const AutoEntry& e) { return e.weight > 0 && (e.cap <= 0.0f || *e.value < e.cap); };
    for (;;) {
        const auto uncapped = int(std::count_if(entries.begin(), entries.end(), open));
        if (uncapped == 0 || uncapped == int(entries.size()) || excess / uncapped < 1) {
            break;
        }
        const int each = excess / uncapped;
        excess -= each * uncapped;
        for (AutoEntry& e : entries) {
            if (open(e)) {
                give(e, each);
            }
        }
    }
    int points = left + excess;
    std::vector<int> working;
    for (const AutoEntry& e : entries) {
        working.push_back(e.weight);
    }
    bool gave = true;
    while (points > 0 && gave) {
        gave = false;
        for (size_t i = 0; i < entries.size() && points > 0; ++i) {
            if (working[i] > 0 && (entries[i].cap <= 0.0f || *entries[i].value < entries[i].cap)) {
                *entries[i].value += 1.0f;
                --working[i];
                --points;
                gave = true;
            }
        }
    }
}

} // namespace

const ActorValueInfo& actor_value_info(int av) { return k_values[std::clamp(av, 0, k_actor_value_count - 1)]; }

int actor_value_by_name(std::string_view name) {
    for (int i = 0; i < k_actor_value_count; ++i) {
        const std::string_view n = k_values[i].name;
        if (n.size() == name.size() && _strnicmp(n.data(), name.data(), n.size()) == 0) {
            return i;
        }
    }
    return -1;
}

ActorRecord actor_record(const forms::Database& db, const Settings& s, uint32_t npc, int level, uint32_t race) {
    ActorRecord r;
    r.npc = npc;
    const forms::FormEntry* entry = db.find(npc);
    if (!entry) {
        return r;
    }
    const forms::Form form(db, *entry);
    const Value* acbs = form.get("ACBS");
    r.acbs_flags = uint32_t(member_int(acbs, "flags"));
    r.level = level > 0 ? level : int(member_int(acbs, "level"));
    r.speed_mult = float(member_int(acbs, "speed_multiplier"));
    if (r.acbs_flags & 0x40000) {
        r.bleedout_override = int(member_int(acbs, "bleedout_override"));
    }
    if (const Value* v = form.get("RNAM")) {
        r.race = v->form;
    }
    if (race) {
        r.race = race;
    }
    if (const Value* v = form.get("CNAM")) {
        r.klass = v->form;
    }
    // Use AI Data (template flag 0x10): the values come down the chain.
    uint32_t ai_source = npc;
    for (int depth = 0; depth < 16; ++depth) {
        const forms::FormEntry* e = db.find(ai_source);
        const forms::Form f = e ? forms::Form(db, *e) : form;
        const Value* flags = f.get("ACBS") ? f.get("ACBS")->path("template_flags") : nullptr;
        const Value* parent = f.get("TPLT");
        const forms::FormEntry* up = parent && parent->form ? db.find(parent->form) : nullptr;
        if (!e || !up || up->type != forms::sig("NPC_") || !flags || !(flags->integer & 0x10)) {
            break;
        }
        ai_source = parent->form;
    }
    const forms::Form ai_form(db, *db.find(ai_source));
    if (const Value* ai = ai_form.get("AIDT")) {
        const char* names[6] = {"aggression", "confidence", "energy_level", "morality", "mood", "assistance"};
        for (int i = 0; i < 6; ++i) {
            r.ai[size_t(i)] = float(member_int(ai, names[i]));
        }
    }
    std::optional<forms::Form> race_form, class_form;
    if (const forms::FormEntry* e = db.find(r.race)) {
        race_form.emplace(db, *e);
    }
    if (const forms::FormEntry* e = db.find(r.klass)) {
        class_form.emplace(db, *e);
    }
    const Value* race_data = race_form ? race_form->get("DATA") : nullptr;
    const Value* class_data = class_form ? class_form->get("DATA") : nullptr;
    if (const forms::FormEntry* klass = r.klass ? db.find(r.klass) : nullptr) {
        const forms::Form k(db, *klass);
        r.bleedout_default = float(member_real(k.get("DATA"), "bleedout_default"));
    }
    const auto start_health = float(member_real(race_data, "starting_health"));
    const auto start_magicka = float(member_real(race_data, "starting_magicka"));
    const auto start_stamina = float(member_real(race_data, "starting_stamina"));
    const auto health_offset = float(member_int(acbs, "health_offset"));
    const auto magicka_offset = float(member_int(acbs, "magicka_offset"));
    const auto stamina_offset = float(member_int(acbs, "stamina_offset"));
    if (const int64_t points = member_int(class_data, "voice_points")) {
        r.voice_points = float(points);
    } else {
        r.voice_points = s.number("iVoicePointsDefault", 50.0f);
    }
    if (r.acbs_flags & 0x10) {
        const int points = r.level - 1;
        r.skills.fill(s.number("iAVDSkillStart", 15.0f));
        if (const Value* boosts = race_data ? race_data->member("skill_boosts") : nullptr) {
            for (const Value& b : boosts->items) {
                const auto av = int(member_int(&b, "skill"));
                if (av >= k_first_skill_value && av < k_first_skill_value + k_skill_value_count) {
                    r.skills[size_t(av - k_first_skill_value)] += float(member_int(&b, "boost"));
                }
            }
        }
        const float cap = s.number("iAVDAutoCalcSkillMax", 100.0f);
        if (const Value* weights = class_data ? class_data->member("skill_weights") : nullptr) {
            std::vector<AutoEntry> entries;
            for (size_t i = 0; i < r.skills.size() && i < weights->items.size(); ++i) {
                entries.push_back({&r.skills[i], int(weights->items[i].integer), cap});
            }
            distribute(std::move(entries), int(s.number("iAVDSkillsLevelUp", 8.0f)) * points);
        }
        const float bonus = npc == 7 ? s.number("fPCHealthLevelBonus", 0.0f) : s.number("fNPCHealthLevelBonus", 5.0f);
        r.health = std::trunc(float(points) * bonus) + health_offset + std::trunc(start_health);
        r.magicka = magicka_offset + std::trunc(start_magicka);
        r.stamina = stamina_offset + std::trunc(start_stamina);
        if (const Value* w = class_data ? class_data->member("attribute_weights") : nullptr) {
            distribute({{&r.health, int(member_int(w, "health")), 0.0f},
                        {&r.magicka, int(member_int(w, "magicka")), 0.0f},
                        {&r.stamina, int(member_int(w, "stamina")), 0.0f}},
                       int(s.number("iAVDhmsLevelUp", 10.0f)) * points);
        }
    } else {
        const Value* dnam = form.get("DNAM");
        const Value* values = dnam ? dnam->member("skill_values") : nullptr;
        const Value* offsets = dnam ? dnam->member("skill_offsets") : nullptr;
        for (size_t i = 0; i < r.skills.size(); ++i) {
            r.skills[i] = float((values && i < values->items.size() ? values->items[i].integer : 0) +
                                (offsets && i < offsets->items.size() ? offsets->items[i].integer : 0));
        }
        r.health = start_health + health_offset;
        r.magicka = start_magicka + magicka_offset;
        r.stamina = start_stamina + stamina_offset;
    }
    return r;
}

float ActorValues::race_value(std::string_view field) const {
    const forms::FormEntry* e = m_db->find(m_record.race);
    if (!e) {
        return 0.0f;
    }
    const forms::Form race(*m_db, *e);
    return float(member_real(race.get("DATA"), field));
}

float ActorValues::record_value(int av) const {
    if (av >= 0 && av < 6) {
        return m_record.ai[size_t(av)];
    }
    if (av >= k_first_skill_value && av < k_first_skill_value + k_skill_value_count) {
        return m_record.skills[size_t(av - k_first_skill_value)];
    }
    switch (av) {
    case av_health: return m_record.health;
    case av_magicka: return m_record.magicka;
    case av_stamina: return m_record.stamina;
    case av_heal_rate: {
        // bHealthRegenFromRacePlayerOnly: compiled 1, an INI setting.
        const bool playable = (int64_t(race_value("flags")) & 1) != 0;
        return !m_player && playable && !m_record.essential_or_protected() ? 0.0f : race_value("health_regen");
    }
    case av_magicka_rate: return race_value("magicka_regen");
    case av_stamina_rate: return race_value("stamina_regen");
    case av_speed_mult: return m_record.speed_mult;
    case av_carry_weight: return race_value("base_carry_weight");
    case av_unarmed_damage: return race_value("unarmed_damage");
    case av_mass: return race_value("base_mass");
    case av_voice_points: return m_record.voice_points;
    case av_voice_rate: return m_settings->number("fVoiceRateBase", 5.0f);
    case av_bow_speed_bonus: return m_settings->number("fDefaultBowSpeedBonus", 1.0f);
    default: return 0.0f;
    }
}

float ActorValues::base(int av) const {
    if (const auto it = m_base.find(av); it != m_base.end()) {
        return it->second;
    }
    const uint32_t flags = actor_value_info(av).flags;
    if (flags & av_flag_default_0) {
        return 0.0f;
    }
    if (flags & av_flag_default_1) {
        return 1.0f;
    }
    if (flags & av_flag_default_100) {
        return 100.0f;
    }
    if ((av == av_inventory_weight || av == av_damage_resist) && derive) {
        return derive(av); // the game clears these as the inventory changes; here they're never stored
    }
    const float value = record_value(av);
    m_base[av] = value;
    return value;
}

float ActorValues::current(int av) const {
    const auto it = m_mods.find(av);
    const float b = base(av);
    return it == m_mods.end() ? b : b + it->second[0] + it->second[1] + it->second[2];
}

float ActorValues::percent(int av) const {
    const float p = permanent(av);
    return p == 0.0f ? 1.0f : current(av) / p;
}

float ActorValues::clamped(int av) const {
    const float v = current(av);
    return actor_value_info(av).flags & av_flag_clamped ? std::clamp(v, 0.0f, 100.0f) : v;
}

void ActorValues::set_base(int av, float value) {
    const float old_current = current(av);
    const float old = base(av);
    m_base[av] = value;
    if (value != old && on_change) {
        on_change(av, old_current, value - old);
    }
}

void ActorValues::mod(Modifier m, int av, float delta) {
    const uint32_t flags = actor_value_info(av).flags;
    const float old_current = current(av);
    if (m == Modifier::damage && delta < 0.0f) {
        if (flags & av_flag_floor_zero) {
            delta = std::max(delta, std::min(-old_current, 0.0f));
        } else if (av == av_health && m_record.essential_or_protected() && !killable_now) {
            delta = old_current <= k_essential_minimum_health ? -0.001f
                                                              : std::max(delta, k_essential_minimum_health - old_current);
        }
    }
    auto& parts = m_mods[av];
    float next = parts[size_t(m)] + delta;
    if (m == Modifier::damage) {
        next = flags & av_flag_damage_positive ? std::max(next, 0.0f) : std::min(next, 0.0f);
    }
    if (std::abs(next) <= 0.0001f) {
        next = 0.0f;
    }
    const float moved = next - parts[size_t(m)];
    parts[size_t(m)] = next;
    if (parts[0] == 0.0f && parts[1] == 0.0f && parts[2] == 0.0f) {
        m_mods.erase(av);
    }
    if (moved != 0.0f && on_change) {
        on_change(av, old_current, moved);
    }
}

void ActorValues::restore(int av, float amount) {
    const float damage = part(av, Modifier::damage);
    if ((amount > 0.0f && damage < 0.0f) || (amount < 0.0f && (actor_value_info(av).flags & av_flag_damage_positive))) {
        mod(Modifier::damage, av, amount);
    }
}

void ActorValues::clear_derived_bases() {
    for (auto it = m_base.begin(); it != m_base.end();) {
        it = actor_value_info(it->first).flags & av_flag_derived ? m_base.erase(it) : std::next(it);
    }
}

void ActorValues::clear_skill_and_attribute_bases() {
    for (int av = k_first_skill_value; av <= av_stamina; ++av) {
        m_base.erase(av);
    }
}

} // namespace spud::game
