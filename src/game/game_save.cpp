// The game's change layer in a save (docs/formats/ost.md): what the player has, and what they changed of the world.

#include "formats/ost.hpp"
#include "game/ai.hpp"
#include "game/game.hpp"

#include <algorithm>
#include <cstring>

namespace spud::game {

namespace {

void put_stack(BinaryWriter& w, const Stack& s) {
    w.write<uint32_t>(s.form);
    w.write<int32_t>(s.count);
    w.write<float>(s.extra.health);
    w.write<uint32_t>(s.extra.owner);
    w.write<uint32_t>(s.extra.enchantment);
    w.write<int32_t>(s.extra.max_charge);
    w.write<float>(s.extra.charge);
    ost::put(w, s.extra.name);
    w.write<uint8_t>(s.extra.soul);
    w.write<uint32_t>(s.extra.poison);
    w.write<int32_t>(s.extra.poison_doses);
    w.write<uint8_t>(uint8_t(s.worn));
    w.write<uint8_t>(s.favorite ? 1 : 0);
}

Stack get_stack(BinaryReader& r) {
    Stack s;
    s.form = r.read<uint32_t>();
    s.count = r.read<int32_t>();
    s.extra.health = r.read<float>();
    s.extra.owner = r.read<uint32_t>();
    s.extra.enchantment = r.read<uint32_t>();
    s.extra.max_charge = r.read<int32_t>();
    s.extra.charge = r.read<float>();
    s.extra.name = ost::get_string(r);
    s.extra.soul = r.read<uint8_t>();
    s.extra.poison = r.read<uint32_t>();
    s.extra.poison_doses = r.read<int32_t>();
    s.worn = Worn(r.read<uint8_t>());
    s.favorite = r.read<uint8_t>() != 0;
    return s;
}

void put_inventory(BinaryWriter& w, const Inventory& inv) {
    w.write<uint32_t>(uint32_t(inv.stacks().size()));
    for (const Stack& s : inv.stacks()) {
        put_stack(w, s);
    }
}

void get_inventory(BinaryReader& r, Inventory& inv) {
    inv.stacks().clear();
    const auto n = r.read<uint32_t>();
    for (uint32_t i = 0; i < n; ++i) {
        inv.stacks().push_back(get_stack(r));
    }
}

} // namespace

void ActorValues::save(BinaryWriter& w) const {
    const auto bases = ost::sorted(m_base);
    w.write<uint32_t>(uint32_t(bases.size()));
    for (const auto& [av, value] : bases) {
        w.write<int32_t>(av);
        w.write<float>(value);
    }
    const auto mods = ost::sorted(m_mods);
    w.write<uint32_t>(uint32_t(mods.size()));
    for (const auto& [av, parts] : mods) {
        w.write<int32_t>(av);
        w.write_span(std::span<const float>(parts));
    }
}

void ActorValues::load(BinaryReader& r) {
    m_base.clear();
    m_mods.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto av = r.read<int32_t>();
        m_base[av] = r.read<float>();
    }
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto av = r.read<int32_t>();
        for (float& v : m_mods[av]) {
            v = r.read<float>();
        }
    }
}

void Character::save(BinaryWriter& w) const {
    m_values.save(w);
    w.write<int32_t>(m_level);
    w.write<float>(m_xp);
    w.write<float>(m_xp_threshold);
    for (const SkillProgress& s : m_skills) {
        w.write<float>(s.level);
        w.write<float>(s.xp);
        w.write<float>(s.threshold);
        w.write<uint32_t>(s.legendary);
    }
    w.write<int32_t>(m_perk_points);
    const auto ranks = ost::sorted(m_ranks);
    w.write<uint32_t>(uint32_t(ranks.size()));
    for (const auto& [perk, rank] : ranks) {
        w.write<uint32_t>(perk);
        w.write<int32_t>(rank);
    }
    const std::vector<uint32_t>& spells = m_target.added_spells();
    w.write<uint32_t>(uint32_t(spells.size()));
    w.write_span(std::span<const uint32_t>(spells));
    w.write<uint32_t>(m_race);
    w.write<uint32_t>(m_original_race);
    w.write<uint8_t>(m_female ? 1 : 0);
    w.write<uint8_t>(m_beast ? 1 : 0);
}

void Character::load(BinaryReader& r) {
    m_values.load(r);
    m_level = r.read<int32_t>();
    m_xp = r.read<float>();
    m_xp_threshold = r.read<float>();
    for (SkillProgress& s : m_skills) {
        s.level = r.read<float>();
        s.xp = r.read<float>();
        s.threshold = r.read<float>();
        s.legendary = r.read<uint32_t>();
    }
    m_perk_points = r.read<int32_t>();
    // Through the rank change, so each perk's entries come and go with it.
    for (const auto& [perk, rank] : ost::sorted(m_ranks)) {
        set_perk_rank(perk, 0);
    }
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto perk = r.read<uint32_t>();
        set_perk_rank(perk, r.read<int32_t>());
    }
    std::vector<uint32_t>& spells = m_target.added_spells();
    spells.resize(r.read<uint32_t>());
    for (uint32_t& spell : spells) {
        spell = r.read<uint32_t>();
    }
    m_race = r.read<uint32_t>();
    m_original_race = r.read<uint32_t>();
    m_female = r.read<uint8_t>() != 0;
    m_beast = r.read<uint8_t>() != 0;
}

void Crime::save(BinaryWriter& w) const {
    w.write<uint32_t>(uint32_t(m_reactions.size()));
    for (const auto& [pair, reaction] : m_reactions) {
        w.write<uint32_t>(pair.first);
        w.write<uint32_t>(pair.second);
        w.write<int32_t>(reaction);
    }
    const auto bounty = ost::sorted(m_bounty);
    w.write<uint32_t>(uint32_t(bounty.size()));
    for (const auto& [faction, b] : bounty) {
        w.write<uint32_t>(faction);
        w.write<Bounty>(b);
    }
    const auto stolen = ost::sorted(m_stolen);
    w.write<uint32_t>(uint32_t(stolen.size()));
    for (const auto& [form, counts] : stolen) {
        w.write<uint32_t>(form);
        w.write_span(std::span<const int32_t>(counts));
    }
    const auto expelled = ost::sorted(m_expelled);
    w.write<uint32_t>(uint32_t(expelled.size()));
    for (const auto& [faction, on] : expelled) {
        w.write<uint32_t>(faction);
        w.write<uint8_t>(on ? 1 : 0);
    }
    w.write<CrimeStats>(m_stats);
}

void Crime::load(BinaryReader& r) {
    m_reactions.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto a = r.read<uint32_t>();
        const auto b = r.read<uint32_t>();
        m_reactions[{a, b}] = r.read<int32_t>();
    }
    m_bounty.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto faction = r.read<uint32_t>();
        m_bounty[faction] = r.read<Bounty>();
    }
    m_stolen.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto form = r.read<uint32_t>();
        auto& counts = m_stolen[form];
        counts[0] = r.read<int32_t>();
        counts[1] = r.read<int32_t>();
    }
    m_expelled.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto faction = r.read<uint32_t>();
        m_expelled[faction] = r.read<uint8_t>() != 0;
    }
    m_stats = r.read<CrimeStats>();
}

template <class T>
void put_optional(BinaryWriter& w, const std::optional<T>& v) {
    w.write<uint8_t>(v ? 1 : 0);
    if (v) {
        w.write<T>(*v);
    }
}

template <class T>
void get_optional(BinaryReader& r, std::optional<T>& v) {
    v.reset();
    if (r.read<uint8_t>() != 0) {
        v = r.read<T>();
    }
}

std::vector<std::byte> Game::save() const {
    BinaryWriter w;
    const Player& p = m_player;
    ost::put(w, p.name);
    put_inventory(w, p.inventory);
    m_character->save(w);
    m_crime.save(w);
    ost::put_set(w, p.books_read);
    const auto effects = ost::sorted(p.ingredient_effects);
    w.write<uint32_t>(uint32_t(effects.size()));
    for (const auto& [ingredient, bits] : effects) {
        w.write<uint32_t>(ingredient);
        w.write<uint16_t>(bits);
    }
    ost::put_set(w, p.incompatible);
    ost::put_set(w, p.known_enchantments);

    const auto containers = ost::sorted(m_containers);
    w.write<uint32_t>(uint32_t(containers.size()));
    for (const auto& [ref, inv] : containers) {
        w.write<uint32_t>(ref);
        put_inventory(w, inv);
    }
    ost::put_set(w, m_taken);
    ost::put_set(w, m_harvested);
    ost::put_set(w, m_unlocked);
    ost::put_set(w, m_player_factions);
    const auto vendors = ost::sorted(m_vendor_stamps);
    w.write<uint32_t>(uint32_t(vendors.size()));
    for (const auto& [ref, stamp] : vendors) {
        w.write<uint32_t>(ref);
        w.write<double>(stamp);
    }
    const auto placed = ost::sorted(m_placed);
    w.write<uint32_t>(uint32_t(placed.size()));
    for (const auto& [ref, item] : placed) {
        w.write<uint32_t>(ref);
        put_stack(w, item.stack);
        w.write_span(std::span<const float>(item.position));
    }
    w.write<uint32_t>(m_next_placed);
    const auto own = ost::sorted(m_own_disabled);
    w.write<uint32_t>(uint32_t(own.size()));
    for (const auto& [ref, disabled] : own) {
        w.write<uint32_t>(ref);
        w.write<uint8_t>(disabled ? 1 : 0);
    }
    const auto stats = ost::sorted(m_stats);
    w.write<uint32_t>(uint32_t(stats.size()));
    for (const auto& [name, value] : stats) {
        ost::put(w, name);
        w.write<int32_t>(value);
    }
    const auto moved = ost::sorted(m_moved);
    w.write<uint32_t>(uint32_t(moved.size()));
    for (const auto& [ref, position] : moved) {
        w.write<uint32_t>(ref);
        w.write<uint32_t>(placed_base(ref));
        for (const float v : position) {
            w.write<float>(v);
        }
    }
    const auto added = m_leveled.added();
    w.write<uint32_t>(uint32_t(added.size()));
    for (const auto& [list, entry] : added) {
        w.write<uint32_t>(list);
        w.write<uint32_t>(entry.form);
        w.write<uint16_t>(entry.level);
        w.write<uint16_t>(entry.count);
    }
    const auto changed = ost::sorted(m_changes);
    w.write<uint32_t>(uint32_t(changed.size()));
    for (const auto& [ref, c] : changed) {
        w.write<uint32_t>(ref);
        w.write<uint32_t>(uint32_t(c.factions.size()));
        for (const auto& [faction, rank] : c.factions) {
            w.write<uint32_t>(faction);
            w.write<int32_t>(rank);
        }
        w.write<uint8_t>(c.left_all_factions ? 1 : 0);
        put_optional(w, c.crime_faction);
        put_optional(w, c.angle);
        put_optional(w, c.scale);
        put_optional(w, c.lock_level);
        put_optional(w, c.locked);
        put_optional(w, c.owner);
        put_optional(w, c.outfit);
        w.write<uint32_t>(c.flags);
        w.write<float>(c.alpha);
        w.write<int32_t>(c.open_state);
    }
    const auto base_flags = ost::sorted(m_base_flags);
    w.write<uint32_t>(uint32_t(base_flags.size()));
    for (const auto& [npc, bits] : base_flags) {
        w.write<uint32_t>(npc);
        w.write<uint32_t>(bits.first);
        w.write<uint32_t>(bits.second);
    }
    const auto deaths = ost::sorted(m_dead_counts);
    w.write<uint32_t>(uint32_t(deaths.size()));
    for (const auto& [npc, count] : deaths) {
        w.write<uint32_t>(npc);
        w.write<int32_t>(count);
    }
    const auto outfits = ost::sorted(m_base_outfits);
    w.write<uint32_t>(uint32_t(outfits.size()));
    for (const auto& [npc, o] : outfits) {
        w.write<uint32_t>(npc);
        w.write<uint32_t>(o.first);
        w.write<uint32_t>(o.second);
    }
    w.write<uint32_t>(m_favor_actor);
    ost::put_set(w, m_known_words);
    w.write<uint32_t>(uint32_t(m_casters.unlocked_words().size()));
    for (const uint32_t word : m_casters.unlocked_words()) {
        w.write<uint32_t>(word);
    }
    w.write<uint32_t>(uint32_t(m_location_data.size()));
    for (const auto& [key, value] : m_location_data) {
        w.write<uint32_t>(key.first);
        w.write<uint32_t>(key.second);
        w.write<float>(value);
    }
    return w.take();
}

void Crime::save_progress(BinaryWriter& w) const {
    const auto stamps = ost::sorted(m_enemy_stamp);
    w.write<uint32_t>(uint32_t(stamps.size()));
    for (const auto& [faction, hours] : stamps) {
        w.write<uint32_t>(faction);
        w.write<double>(hours);
    }
    const auto resisting = ost::sorted(m_resist_clock);
    w.write<uint32_t>(uint32_t(resisting.size()));
    for (const auto& [faction, seconds] : resisting) {
        w.write<uint32_t>(faction);
        w.write<double>(seconds);
    }
    const auto changes = ost::sorted(m_relationship_changes);
    w.write<uint32_t>(uint32_t(changes.size()));
    for (const auto& [pair, rank] : changes) {
        w.write<uint64_t>(pair);
        w.write<int32_t>(rank);
    }
    w.write<uint32_t>(uint32_t(m_crimes.size()));
    for (const CrimeRecord& c : m_crimes) {
        w.write<uint8_t>(uint8_t(c.type));
        w.write<uint32_t>(c.faction);
        w.write<uint32_t>(c.target);
        w.write<uint32_t>(c.owner);
        w.write<uint32_t>(c.form);
        w.write<int32_t>(c.count);
        w.write<int32_t>(c.gold);
        w.write<double>(c.age);
        w.write<uint8_t>(uint8_t(c.settled | c.charged << 1));
        w.write<uint32_t>(uint32_t(c.witnesses.size()));
        w.write_span(std::span<const uint32_t>(c.witnesses));
    }
    w.write<uint32_t>(uint32_t(m_warnings.size()));
    for (const Warning& warning : m_warnings) {
        w.write<uint32_t>(uint32_t(warning.crime));
        w.write<float>(warning.seconds);
    }
    w.write<int32_t>(m_warning_count);
    w.write<float>(m_warning_timer);
    w.write<int32_t>(m_attack_next_day);
    w.write<uint32_t>(m_jail_faction);
    w.write<int32_t>(m_days);
    w.write<uint8_t>(m_escaping ? 1 : 0);
    w.write<uint32_t>(m_trespass_owner);
    w.write<double>(m_hours);
    w.write<double>(m_clock);
    w.write<float>(m_sweep);
}

void Crime::load_progress(BinaryReader& r) {
    m_enemy_stamp.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto faction = r.read<uint32_t>();
        m_enemy_stamp[faction] = r.read<double>();
    }
    m_resist_clock.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto faction = r.read<uint32_t>();
        m_resist_clock[faction] = r.read<double>();
    }
    m_relationship_changes.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto pair = r.read<uint64_t>();
        m_relationship_changes[pair] = r.read<int32_t>();
    }
    m_crimes.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        CrimeRecord& c = m_crimes.emplace_back();
        c.type = CrimeType(r.read<uint8_t>());
        c.faction = r.read<uint32_t>();
        c.target = r.read<uint32_t>();
        c.owner = r.read<uint32_t>();
        c.form = r.read<uint32_t>();
        c.count = r.read<int32_t>();
        c.gold = r.read<int32_t>();
        c.age = r.read<double>();
        const auto bits = r.read<uint8_t>();
        c.settled = bits & 1;
        c.charged = bits & 2;
        c.witnesses.resize(r.read<uint32_t>());
        for (uint32_t& witness : c.witnesses) {
            witness = r.read<uint32_t>();
        }
    }
    m_warnings.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        Warning& warning = m_warnings.emplace_back();
        warning.crime = r.read<uint32_t>();
        warning.seconds = r.read<float>();
    }
    m_warning_count = r.read<int32_t>();
    m_warning_timer = r.read<float>();
    m_attack_next_day = r.read<int32_t>();
    m_jail_faction = r.read<uint32_t>();
    m_days = r.read<int32_t>();
    m_escaping = r.read<uint8_t>() != 0;
    m_trespass_owner = r.read<uint32_t>();
    m_hours = r.read<double>();
    m_clock = r.read<double>();
    m_sweep = r.read<float>();
}

void MagicTarget::save(BinaryWriter& w) const {
    w.write<uint32_t>(commander);
    w.write<uint8_t>(uint8_t(fed_on | soul_taken << 1 | m_ghost << 2));
    w.write<uint32_t>(uint32_t(m_spells.size()));
    w.write_span(std::span<const uint32_t>(m_spells));
    const auto count = std::ranges::count_if(m_effects, [](const ActiveEffect& e) { return !e.finished; });
    w.write<uint32_t>(uint32_t(count));
    for (const ActiveEffect& e : m_effects) {
        if (e.finished) {
            continue;
        }
        w.write<uint32_t>(e.id);
        w.write<uint32_t>(e.spell);
        w.write<uint32_t>(e.effect);
        w.write<uint32_t>(e.caster);
        w.write<uint32_t>(e.object);
        w.write<int32_t>(e.item && e.source ? int32_t(e.source - e.item->effects.data()) : -1);
        for (const float v : {e.magnitude, e.duration, e.elapsed, e.applied, e.fraction, e.timer}) {
            w.write<float>(v);
        }
        w.write<Units3>(e.location);
        w.write<uint8_t>(e.casting_source);
        w.write<uint16_t>(uint16_t(e.has_location | e.has_conditions << 1 | e.no_expiry << 2 | e.tethered << 3 | e.worn << 4 |
                                   e.dual << 5 | e.active << 6 | e.dispelled << 7 | e.suppressed << 8 | e.skill_paid << 9 |
                                   e.first_update << 10 | e.started << 11));
        w.write<uint32_t>(e.link);
    }
}

void MagicTarget::load(BinaryReader& r) {
    commander = r.read<uint32_t>();
    const auto bits = r.read<uint8_t>();
    fed_on = bits & 1;
    soul_taken = bits & 2;
    m_ghost = bits & 4;
    m_spells.resize(r.read<uint32_t>());
    for (uint32_t& spell : m_spells) {
        spell = r.read<uint32_t>();
    }
    m_effects.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        ActiveEffect e;
        e.id = r.read<uint32_t>();
        e.target = m_ref;
        e.spell = r.read<uint32_t>();
        e.effect = r.read<uint32_t>();
        e.caster = r.read<uint32_t>();
        e.object = r.read<uint32_t>();
        const auto source = r.read<int32_t>();
        for (float* v : {&e.magnitude, &e.duration, &e.elapsed, &e.applied, &e.fraction, &e.timer}) {
            *v = r.read<float>();
        }
        e.location = r.read<Units3>();
        e.casting_source = r.read<uint8_t>();
        const auto flags = r.read<uint16_t>();
        e.has_location = flags & 1;
        e.has_conditions = flags & 2;
        e.no_expiry = flags & 4;
        e.tethered = flags & 8;
        e.worn = flags & 16;
        e.dual = flags & 32;
        e.active = flags & 64;
        e.dispelled = flags & 128;
        e.suppressed = flags & 256;
        e.skill_paid = flags & 512;
        e.first_update = flags & 1024;
        e.started = flags & 2048;
        e.link = r.read<uint32_t>();
        e.item = m_game->magic().spell(e.spell);
        e.setting = m_game->magic().effect(e.effect);
        if (e.item && source >= 0 && size_t(source) < e.item->effects.size()) {
            e.source = &e.item->effects[size_t(source)];
        }
        if (e.setting) { // a plugin that's gone takes its effects with it
            m_effects.push_back(e);
        }
    }
}

void Ai::save(BinaryWriter& w) const {
    std::vector<const Agent*> away;
    for (const uint32_t ref : m_order) {
        const Agent& a = m_agents.at(ref);
        const bool moved = a.space != a.home_space || std::memcmp(&a.position, &a.home, sizeof(NavPoint)) != 0;
        if (moved || a.disabled || a.detach_hours || ref >= k_created_refs) {
            away.push_back(&a);
        }
    }
    w.write<uint32_t>(uint32_t(away.size()));
    for (const Agent* a : away) {
        w.write<uint32_t>(a->ref);
        w.write<uint32_t>(a->npc);
        w.write<uint32_t>(a->space);
        w.write<NavPoint>(a->position);
        w.write<float>(a->heading);
        w.write<float>(a->scale);
        w.write<uint8_t>(a->disabled ? 1 : 0);
        w.write<uint32_t>(a->detach_hours);
    }
}

void Ai::apply_saved() {
    if (m_saved.empty()) {
        return;
    }
    // Everyone back to the records first: what isn't in the save hadn't moved.
    std::erase_if(m_order, [this](uint32_t ref) {
        if (ref < k_created_refs) {
            return false;
        }
        m_agents.erase(ref);
        return true;
    });
    for (const uint32_t ref : m_order) {
        Agent& a = m_agents.at(ref);
        place_at(ref, a.home_space, a.home, a.heading);
        a.disabled = false;
        a.detach_hours = 0;
    }
    BinaryReader r(m_saved);
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto ref = r.read<uint32_t>();
        const auto npc = r.read<uint32_t>();
        const auto space = r.read<uint32_t>();
        const auto position = r.read<NavPoint>();
        const auto heading = r.read<float>();
        const auto scale = r.read<float>();
        const bool disabled = r.read<uint8_t>() != 0;
        const auto detach_hours = r.read<uint32_t>();
        if (!m_agents.contains(ref)) {
            if (ref < k_created_refs) {
                continue; // a plugin that's gone
            }
            add(ref, npc, space, position, heading, scale);
        }
        place_at(ref, space, position, heading);
        Agent& a = m_agents.at(ref);
        a.disabled = disabled;
        a.detach_hours = detach_hours;
    }
    m_saved.clear();
    ++m_roster;
}

std::vector<std::byte> Game::save_actors() const {
    BinaryWriter w;
    std::vector<uint32_t> refs;
    for (const auto& [ref, values] : m_actor_values) {
        refs.push_back(ref);
    }
    std::ranges::sort(refs);
    w.write<uint32_t>(uint32_t(refs.size()));
    for (const uint32_t ref : refs) {
        w.write<uint32_t>(ref);
        m_actor_values.at(ref).save(w);
    }
    refs.clear();
    for (const auto& [ref, target] : m_magic_targets) {
        refs.push_back(ref);
    }
    std::ranges::sort(refs);
    w.write<uint32_t>(uint32_t(refs.size()));
    for (const uint32_t ref : refs) {
        w.write<uint32_t>(ref);
        m_magic_targets.at(ref)->save(w);
    }
    m_character->magic_target().save(w);
    w.write<uint32_t>(m_next_effect);
    const auto worn = ost::sorted(m_worn_effects);
    w.write<uint32_t>(uint32_t(worn.size()));
    for (const auto& [enchantment, count] : worn) {
        w.write<uint32_t>(enchantment);
        w.write<int32_t>(count);
    }
    m_crime.save_progress(w);
    if (m_ai) {
        m_ai->save(w);
    } else {
        w.write<uint32_t>(0);
    }
    return w.take();
}

void Game::load_actors(std::span<const std::byte> bytes) {
    BinaryReader r(bytes);
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        actor_values(r.read<uint32_t>()).load(r);
    }
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        magic_target(r.read<uint32_t>()).load(r);
    }
    m_character->magic_target().load(r);
    m_next_effect = r.read<uint32_t>();
    m_worn_effects.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto enchantment = r.read<uint32_t>();
        m_worn_effects[enchantment] = r.read<int32_t>();
    }
    m_crime.load_progress(r);
    const std::span<const std::byte> rest = bytes.subspan(bytes.size() - r.remaining());
    ai().set_saved({rest.begin(), rest.end()});
}

void Game::load(std::span<const std::byte> bytes) {
    BinaryReader r(bytes);
    Player& p = m_player;
    p.name = ost::get_string(r);
    get_inventory(r, p.inventory);
    m_character->load(r);
    m_crime.load(r);
    ost::get_set(r, p.books_read);
    p.ingredient_effects.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto ingredient = r.read<uint32_t>();
        p.ingredient_effects[ingredient] = r.read<uint16_t>();
    }
    ost::get_set(r, p.incompatible);
    ost::get_set(r, p.known_enchantments);

    m_containers.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto ref = r.read<uint32_t>();
        get_inventory(r, m_containers[ref]);
    }
    ost::get_set(r, m_taken);
    ost::get_set(r, m_harvested);
    ost::get_set(r, m_unlocked);
    ost::get_set(r, m_player_factions);
    m_vendor_stamps.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto ref = r.read<uint32_t>();
        m_vendor_stamps[ref] = r.read<double>();
    }
    m_placed.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto ref = r.read<uint32_t>();
        Placed item;
        item.stack = get_stack(r);
        for (float& v : item.position) {
            v = r.read<float>();
        }
        m_placed[ref] = std::move(item);
    }
    m_next_placed = r.read<uint32_t>();

    m_own_disabled.clear();
    m_ref_overrides.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto ref = r.read<uint32_t>();
        m_own_disabled[ref] = r.read<uint8_t>() != 0;
    }
    for (const auto& [ref, disabled] : ost::sorted(m_own_disabled)) {
        publish_ref_state(ref);
    }
    m_stats.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        std::string name = ost::get_string(r);
        m_stats[std::move(name)] = r.read<int32_t>();
    }
    m_moved.clear();
    m_placed_forms.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto ref = r.read<uint32_t>();
        if (const auto base = r.read<uint32_t>()) {
            m_placed_forms[ref] = base;
        }
        auto& position = m_moved[ref];
        for (float& v : position) {
            v = r.read<float>();
        }
    }
    m_leveled.clear_added();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto list = r.read<uint32_t>();
        const auto form = r.read<uint32_t>();
        const auto level = r.read<uint16_t>();
        m_leveled.add_form(list, form, level, r.read<uint16_t>());
    }
    m_changes.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        RefChanges& c = m_changes[r.read<uint32_t>()];
        for (auto f = r.read<uint32_t>(); f > 0; --f) {
            const auto faction = r.read<uint32_t>();
            c.factions[faction] = r.read<int32_t>();
        }
        c.left_all_factions = r.read<uint8_t>() != 0;
        get_optional(r, c.crime_faction);
        get_optional(r, c.angle);
        get_optional(r, c.scale);
        get_optional(r, c.lock_level);
        get_optional(r, c.locked);
        get_optional(r, c.owner);
        get_optional(r, c.outfit);
        c.flags = r.read<uint32_t>();
        c.alpha = r.read<float>();
        c.open_state = r.read<int32_t>();
    }
    m_base_flags.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        auto& bits = m_base_flags[r.read<uint32_t>()];
        bits.first = r.read<uint32_t>();
        bits.second = r.read<uint32_t>();
    }
    m_dead_counts.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto npc = r.read<uint32_t>();
        m_dead_counts[npc] = r.read<int32_t>();
    }
    m_base_outfits.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        auto& o = m_base_outfits[r.read<uint32_t>()];
        o.first = r.read<uint32_t>();
        o.second = r.read<uint32_t>();
    }
    for (const auto& [npc, o] : ost::sorted(m_base_outfits)) {
        if (o.first && on_outfit) {
            on_outfit(npc, o.first);
        }
    }
    m_favor_actor = r.read<uint32_t>();
    ost::get_set(r, m_known_words);
    m_casters.clear_words();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        m_casters.unlock_word(r.read<uint32_t>());
    }
    m_location_data.clear();
    for (auto n = r.read<uint32_t>(); n > 0; --n) {
        const auto location = r.read<uint32_t>();
        const auto keyword = r.read<uint32_t>();
        m_location_data[{location, keyword}] = r.read<float>();
    }
}

} // namespace spud::game
