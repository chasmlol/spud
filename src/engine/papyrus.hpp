#pragma once

// Papyrus in the frame: the VM over scripts/scripts.oss, driven the way Main::Update drives it (
// 1.3): one VM update and dispatch with the frame's work, a second dispatch after render. Nothing runs until a game
// system or a `papyrus` script command starts something.

#include "engine/game_clock.hpp"
#include "game/dialogue.hpp"
#include "game/quests.hpp"
#include "script/natives.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <map>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spud {

class Papyrus final : public game::ConditionFacts {
public:
    // Reads the cache; throws if the scripts or forms can't be read.
    Papyrus(const std::filesystem::path& cache, GameClock& clock);
    ~Papyrus();

    void update(float dt_seconds, bool menu_mode);
    void after_render();
    // "Script.Function": a global function, with no arguments.
    bool call(std::string_view target);

    // Binds quest scripts and starts the start-game quests.
    void new_game();
    game::QuestSystem* quests() { return m_quests.get(); }
    game::QuestWorld& quest_world() { return *m_quest_world; }
    game::Dialogue* dialogue() { return m_dialogue.get(); }
    void attach_dialogue(game::DialogueOutput& output);
    game::Conditions& conditions() { return *m_conditions; }
    game::WorldState& world() { return m_world; }
    // Quests' and globals' answers to conditions; the game asks here for what it doesn't know.
    bool condition_value(uint16_t function, uint32_t subject, uint32_t target, uint32_t param1, uint32_t param2,
                         double& out) override;
    bool global_value(uint32_t global, float& out) override;
    bool quest_variable(uint32_t quest, const std::string& name, double& out) override;
    void set_global(uint32_t global, float value);
    // What an item's move tells: the item's own scripts, then the containers and their aliases.
    void item_moved(uint32_t form, int32_t count, uint32_t from, uint32_t to);
    void add_inventory_filter(script::Handle handle, uint32_t form) { m_inventory_filters[handle].push_back(form); }
    void remove_inventory_filter(script::Handle handle, uint32_t form);
    void clear_inventory_filters(script::Handle handle) { m_inventory_filters.erase(handle); }
    // An event for a reference's scripts, bound on first need, and the aliases that hold it.
    void ref_event(uint32_t ref, std::string_view name, std::vector<script::Value> args = {});
    // The scripts of an item in `holder`'s inventory.
    void item_event(uint32_t holder, uint32_t form, std::string_view name, std::vector<script::Value> args);
    std::function<std::vector<uint32_t>(uint32_t ref)> effects_on; // the active effects' ids
    void trigger_event(uint32_t ref, bool entered, uint32_t who);
    // A book is read: OnRead on its scripts (the Black Books start here).
    void read_event(uint32_t book);
    // A reference's scripts, its base's first, and the event that wakes them.
    void activate(uint32_t ref, uint32_t base, uint32_t activator);
    // The MGEF's scripts on one running effect, and the events they wait for.
    // OnHit on the victim and its aliases.
    void hit_event(uint32_t victim, uint32_t attacker, uint32_t source, uint32_t projectile, uint32_t hit_flags);
    void effect_started(uint32_t id, uint32_t effect, uint32_t caster, uint32_t target = script::k_player_ref);
    void effect_finished(uint32_t id, uint32_t caster, uint32_t target = script::k_player_ref);
    // An event for the objects registered for it.
    void send_sleep(const std::unordered_set<uint64_t>& listeners, std::string_view name, std::vector<script::Value> args);
    // An actor event for the player's own scripts and the aliases that hold them (OnRaceSwitchComplete).
    void player_event(std::string_view name, std::vector<script::Value> args = {});
    // A perk's Activate choice ran its script fragment (the PerkEntryRun event).
    void run_perk_fragment(uint32_t target, uint32_t perk, int fragment);

    script::VirtualMachine& vm() { return *m_vm; }
    script::GameHost::WorldCalls& world_calls() { return m_host.world; }
    script::GameHost& host() { return m_host; }
    // OnPlayerFastTravelEnd(game hours) to what has registered for it.
    void fast_travel_ended(float hours);
    // A trigger volume's event, its scripts bound on first use: OnTriggerEnter and Leave with the player, OnCellLoad.

private:
    GameClock& m_clock;
    oss::Scripts m_scripts;
    std::unique_ptr<forms::Database> m_forms;
    script::GameHost m_host;
    std::unique_ptr<script::VirtualMachine> m_vm;
    game::WorldState m_world;
    std::unique_ptr<game::Conditions> m_conditions;
    std::unique_ptr<game::QuestWorld> m_quest_world;
    std::unique_ptr<game::QuestSystem> m_quests;
    std::unique_ptr<game::Dialogue> m_dialogue;
    double m_clock_ms = 0;
    std::vector<script::Handle> m_finished_effects; // unbound once their OnEffectFinish has run
    std::unordered_map<script::Handle, std::vector<uint32_t>> m_inventory_filters;
    // The scripted items of an inventory: (container, base) to their handles.
    std::map<std::pair<uint32_t, uint32_t>, std::vector<script::Handle>> m_item_handles;
    std::unordered_set<uint32_t> m_bound_bases;
    uint32_t m_next_item = 1;
    uint64_t m_last_ms = 0;
};

} // namespace spud
