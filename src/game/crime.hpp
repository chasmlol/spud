#pragma once

// Crime: who saw what, bounty per crime faction, stolen goods, jail, and relationships.
// Detection is AI's: `witnesses` asks the app who is around and whether they see the player.

#include "formats/forms_typed.hpp"
#include "game/inventory.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace spud::game {

class Game;

enum class CrimeType : uint8_t { steal, pickpocket, trespass, attack, murder, escape, werewolf };

enum FactionFlag : uint32_t {
    faction_expelled = 0x4,
    faction_enemy = 0x8, // PC enemy, kept here at run time
    faction_tracks_crime = 0x40,
    faction_ignore_murder = 0x80,
    faction_ignore_assault = 0x100,
    faction_ignore_steal = 0x200,
    faction_ignore_trespass = 0x400,
    faction_no_report_members = 0x800,
    faction_default_gold = 0x1000,
    faction_ignore_pickpocket = 0x2000,
    faction_can_be_owner = 0x8000,
    faction_ignore_werewolf = 0x10000,
};

// A crime faction's record (FACT), its CRVA values after the defaults flag.
struct CrimeFaction {
    uint32_t form = 0, flags = 0;
    std::string name;
    int32_t murder = 0, assault = 0, trespass = 0, pickpocket = 0, escape = 0, werewolf = 0;
    float steal_mult = 0;
    uint32_t jail_marker = 0, follower_marker = 0, stolen_container = 0, player_container = 0, outfit = 0, group = 0;
};

struct Bounty {
    int32_t violent = 0, non_violent = 0;
    int32_t infamy_violent = 0, infamy_non_violent = 0;
    int32_t total() const { return std::max(violent, 0) + std::max(non_violent, 0); }
};

// An actor near the player, as the app knows them.
struct Witness {
    uint32_t ref = 0, npc = 0;
    bool detects = false; // AI's detection: the player is seen
    bool alive = true;
};

struct CrimeRecord {
    CrimeType type = CrimeType::steal;
    uint32_t faction = 0, target = 0, owner = 0;
    uint32_t form = 0; // what was stolen
    int32_t count = 0, gold = 0;
    double age = 0;    // real seconds
    bool settled = false, charged = false;
    std::vector<uint32_t> witnesses; // references
};

// Where a jail move goes: the reference to arrive at and how.
struct JailMove {
    uint32_t door = 0;                    // the door on the arriving side
    std::array<float, 3> position{};      // Skyrim units
    float heading = 0;                    // radians
};

struct CrimeStats {
    int32_t items_stolen = 0, trespasses = 0, assaults = 0, murders = 0, pockets_picked = 0;
    int32_t times_jailed = 0, days_jailed = 0, fines_paid = 0, jail_escapes = 0;
    int32_t largest_bounty = 0, lifetime_bounty = 0;
};

class Crime {
public:
    explicit Crime(Game& game) : m_game(game) {}

    // Who is around, for the crime just committed. Set by the app.
    std::function<std::vector<Witness>()> witnesses;
    // Game hours pass in jail.
    std::function<void(double hours)> pass_hours;
    bool reporting = true; // Game.SetPlayerReportCrime

    const CrimeFaction* faction(uint32_t form);
    // An NPC_'s crime faction (CRIF).
    uint32_t crime_faction_of(uint32_t npc) const;
    bool in_faction(uint32_t npc, uint32_t faction) const;
    bool is_guard(uint32_t npc) const;

    // Reports. `value` is the argument the game passes with no form; the count's base value otherwise.
    void report_theft(uint32_t target, uint32_t form, int32_t count, int32_t value, uint32_t owner, bool can_warn);
    void report_pickpocket(uint32_t npc_ref, uint32_t npc);
    void report_trespass(uint32_t owner);
    void report_attack(uint32_t victim_ref, uint32_t victim_npc);
    void report_murder(uint32_t victim_ref, uint32_t victim_npc);

    // Bounty by faction (the game's virtuals and on).
    const Bounty& bounty(uint32_t faction) { return m_bounty[faction]; }
    int32_t crime_gold(uint32_t faction) { return bounty(faction).total(); }
    void set_crime_gold(uint32_t faction, bool violent, int32_t gold);
    void mod_crime_gold(uint32_t faction, bool violent, int32_t delta);
    void adjust_crime_gold(uint32_t faction, bool violent, int32_t delta);
    void reset_crime_gold(uint32_t faction);
    int32_t stolen_value(uint32_t faction, bool with_crime) const;

    // Enemy state and arrest (AI and Dialogue ask).
    void set_enemy(uint32_t faction, bool enemy);
    bool is_enemy(uint32_t faction);
    bool arrest_on_sight(uint32_t guard_faction);
    bool attack_on_sight(uint32_t guard_faction);
    void set_resisting_arrest(uint32_t faction);
    bool resisting_arrest(uint32_t faction);
    void set_expelled(uint32_t faction, bool expelled);
    bool expelled(uint32_t faction);

    // Fines and jail.
    void pay_fine(uint32_t faction, bool go_to_jail, bool remove_stolen);
    void go_to_jail(uint32_t faction, bool remove_inventory, bool real_jail);
    void serve_sentence();
    void clear_prison() { clear_jail_state(); } // no faction, no sentence
    void set_escaping(bool escaping, bool confirmed);
    int32_t jail_days() const { return m_days; }
    uint32_t jail_faction() const { return m_jail_faction; }
    bool escaping() const { return m_escaping; }
    // Where the player is to be moved, once.
    std::optional<JailMove> take_move();

    // Relationships: the rank (-4 archnemesis .. 4 lover) between two NPC_s, and what a friend lets go.
    int relationship(uint32_t a, uint32_t b) const;
    void set_relationship(uint32_t a, uint32_t b, int rank);
    int favor_value(uint32_t npc) const; // towards the player
    // The bribe a guard asks.
    int32_t bribe_amount(uint32_t npc) const;
    // The faction reaction of one to another: 0 neutral, 1 enemy, 2 ally, 3 friend (XNAM).
    int faction_reaction(uint32_t a, uint32_t b) const;
    void set_faction_reaction(uint32_t a, uint32_t b, int reaction); // Faction.SetAlly, SetEnemy

    // Trespass: the cell the player entered (0: none).
    void enter_cell(uint32_t cell);
    bool trespassing() const { return m_trespass_owner != 0; }

    // Each frame, real seconds and game hours passed so far: crimes age and end, warnings run out.
    void update(float seconds, double game_hours);

    const CrimeStats& stats() const { return m_stats; }
    void save(BinaryWriter& w) const; // game_save.cpp
    void load(BinaryReader& r);
    // What's still under way: crimes being judged, warnings, jail.
    size_t crimes_under_way() const { return m_crimes.size(); }
    void save_progress(BinaryWriter& w) const;
    void load_progress(BinaryReader& r);
    const std::vector<CrimeRecord>& records() const { return m_crimes; }
    const std::unordered_map<uint32_t, Bounty>& bounties() const { return m_bounty; }

private:
    std::map<std::pair<uint32_t, uint32_t>, int> m_reactions; // what scripts set, over XNAM
    int32_t gold_of(const CrimeRecord& crime);
    bool ignored(uint32_t witness_npc, CrimeType type, uint32_t target_npc) const;
    bool shares_group(uint32_t npc, uint32_t faction);
    void take_over(CrimeRecord& crime, const std::vector<Witness>& around);
    void charge(CrimeRecord& crime);
    void add_no_crime(uint32_t target, int32_t value);
    void clear_jail_state();
    void release();
    std::optional<JailMove> door_move(uint32_t door) const;
    void move_all(Inventory& from, Inventory& to, const std::function<bool(const Stack&)>& which);

    Game& m_game;
    std::unordered_map<uint32_t, CrimeFaction> m_factions;
    std::unordered_map<uint32_t, Bounty> m_bounty;
    std::unordered_map<uint32_t, std::array<int32_t, 2>> m_stolen; // no crime, crime
    std::unordered_map<uint32_t, double> m_enemy_stamp;            // game hours
    std::unordered_map<uint32_t, double> m_resist_clock;           // real seconds
    std::unordered_map<uint32_t, bool> m_expelled;
    std::unordered_map<uint64_t, int> m_relationship_changes;
    std::vector<CrimeRecord> m_crimes;
    struct Warning {
        size_t crime = 0;
        float seconds = 0;
    };
    std::vector<Warning> m_warnings;
    int m_warning_count = 0;
    float m_warning_timer = 0;
    int m_attack_next_day = -1; // the day attack-on-next-theft was stamped, -1 for none
    uint32_t m_jail_faction = 0;
    int32_t m_days = 0;
    bool m_escaping = false;
    std::optional<JailMove> m_move;
    uint32_t m_trespass_owner = 0;
    double m_hours = 0, m_clock = 0;
    float m_sweep = 0;
    CrimeStats m_stats;
};

} // namespace spud::game
