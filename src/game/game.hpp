#pragma once

// The running game's item side: the player's inventory and skills, the world's containers and the references the
// player has changed, over the forms database. Knows nothing of the renderer or the UI, so it runs headless.

#include "formats/forms.hpp"
#include "game/caster.hpp"
#include "game/character.hpp"
#include "game/combat.hpp"
#include "game/combat_ai.hpp"
#include "game/idles.hpp"
#include "game/camera_paths.hpp"
#include "game/conditions.hpp"
#include "game/detection.hpp"
#include "game/inventory.hpp"
#include "game/items.hpp"
#include "game/ai.hpp"
#include "game/crafting.hpp"
#include "game/crime.hpp"
#include "game/leveled.hpp"
#include "game/magic.hpp"
#include "game/navmesh.hpp"
#include "game/player.hpp"
#include "game/ref_changes.hpp"
#include "game/impacts.hpp"
#include "game/projectiles.hpp"
#include "game/settings.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spud::game {

enum class TargetKind : uint8_t { none, item, container, furniture, flora, door, actor, activator };

// A reference as the crosshair sees it.
struct Target {
    TargetKind kind = TargetKind::none;
    uint32_t ref = 0, base = 0;
    std::string verb, name; // "Take", "Iron Sword"
    std::string detail;     // a container's third line: Empty, or the lock
    int32_t count = 1;      // XCNT
    uint32_t owner = 0;     // NPC_ or FACT, 0 for nobody
    bool steal = false;     // taking it is a crime
    uint8_t lock_level = 0; // XLOC, 0 when not locked
    uint32_t key = 0;
    bool locked() const { return lock_level != 0; }
    bool bed = false; // furniture with a sleep marker
    bool mount = false; // an actor to ride
    bool hostile = false; // an actor fighting the player: the HUD shows no prompt
    bool corpse = false;  // a dead actor: Search, and the container menu
    const Item* item = nullptr; // TargetKind::item
};

// A HUD notification, or a sound alone, a game action leaves for the UI to show.
struct Notice {
    std::string text, sound;
    // A skill increase: the skill's name and new level with the level meter's XP.
    std::string skill;
    int skill_level = 0, player_level = 0;
    float xp = 0;
};

// What riding needs of a mount: its race's mount data and its bound.
struct MountData {
    std::array<float, 3> mount_offset{}, dismount_offset{}, camera_offset{}; // units, x right, y ahead
    std::array<float, 3> half{};  // units
    float stamina = 100, stamina_regen = 10;
    bool horse = false;
};

enum class ItemUse : uint8_t { nothing, changed, read };

// A lock's numbers for the player now: the sweet spot, the partial zone, the seconds a pick lasts.
struct LockNumbers {
    float size = 0, partial = 0, break_seconds = 0;
};

class QuestSystem;

class Game final : public ConditionFacts, public InventoryListener {
public:
    explicit Game(const std::filesystem::path& forms_file);

    const forms::Database& db() const { return *m_db; }
    const Settings& settings() const { return m_settings; }
    const Items& items() const { return m_items; }
    const Magic& magic() const { return m_magic; }
    Player& player() { return m_player; }
    Character& character() { return *m_character; }
    QuestSystem* quests() { return m_quest_system; }
    void set_quests(QuestSystem* quests) { m_quest_system = quests; }
    // The values of an actor: the player's character, or a placed actor's or an NPC_'s own, made from its record.
    ActorValues& actor_values(uint32_t ref);
    const Character& character() const { return *m_character; }
    Conditions& conditions() { return m_conditions; }
    // What actors are doing now: the functions of by number.
    bool live_condition(uint16_t fn, uint32_t subject, uint32_t p1, uint32_t p2, double& out);
    bool graph_value(uint32_t ref, const std::string& name, double& out) override {
        int32_t value = 0;
        const bool found = m_combat.world().graph_value(ref, name, value);
        out = value;
        return found;
    }
    bool graph_real(uint32_t ref, const std::string& name, double& out) override {
        float value = 0;
        const bool found = m_combat.world().graph_real(ref, name, value);
        out = value;
        return found;
    }
    void set_player_riding(bool riding) { m_player_riding = riding; }
    bool m_player_riding = false;
    // CreateDetectionEvent and what else makes a noise the player answers for; detection is only worked out against the player.
    DetectionEvent& player_detection_event() { return m_detection_event; }
    WorldState& world() { return m_world; }
    // The running game's answers to conditions: the player's character and inventory.
    bool condition_value(uint16_t function, uint32_t subject, uint32_t target, uint32_t param1, uint32_t param2,
                         double& out) override;
    bool global_value(uint32_t global, float& out) override;
    // The magic and life functions, for any subject (magic_target.cpp's neighbour in game.cpp).
    bool magic_condition(uint16_t function, uint32_t subject, uint32_t param1, double& out);
    // What the player's Quests, globals and clock answer: set by the engine once scripts run.
    ConditionFacts* script_facts = nullptr;
    Crafting& crafting() { return m_crafting; }
    // Constant-effect enchantments follow what the player wears; each frame.
    void update_worn_effects();
    Crime& crime() { return m_crime; }
    Combat& combat() { return m_combat; }
    CombatAi& combat_ai() { return m_combat_ai; }
    CameraPaths& camera_paths() { return m_camera_paths; }
    VatsEntry& vats() { return m_vats; }
    // Body part 0's node for that actor, lower case: where the kill camera hangs. "" for none.
    std::string body_part_node(uint32_t ref);
    Idles& idles() { return m_idles; }
    Projectiles& projectiles() { return m_projectiles; }
    Impacts& impacts() { return m_impacts; }
    // The game for Health: what the player takes, or what anyone else does, at difficulty 0..5.
    float damage_mult(int difficulty, bool to_player) const;
    bool is_child(uint32_t npc) const;
    std::string display_name_of(uint32_t npc);
    // Half the controller's bound, in units: what an actor stands in, as the crosshair sees them.
    std::array<float, 3> body_half(uint32_t npc);
    // The base has flag 0x100000: friendly hits never count.
    bool ignores_friendly_hits(uint32_t npc) const;
    float soul_value_of(uint32_t npc) const;
    // The game for a reference: 0 once its soul is gone, 1 petty. 5 grand.
    int soul_level_of(uint32_t ref);
    bool has_npc_soul(uint32_t ref) const;
    float soul_level_value(int level) const;
    // A reference's magic: any actor's, or an object's (a door, a hazard's source).
    MagicTarget& magic_target(uint32_t ref);
    Casters& casters() { return m_casters; }
    MagicWorld& magic_world() { return *m_magic_world; }
    void set_magic_world(MagicWorld* world) { m_magic_world = world ? world : &m_no_magic_world; }
    bool is_actor(uint32_t ref) const;
    uint32_t next_effect_id() { return m_next_effect++; }
    // The target whose list holds a running effect, null when it's gone.
    MagicTarget* effect_target(uint32_t id);
    // Each frame, after the player's: every other target's effects, then the casters.
    void update_magic(float dt);
    // The actors' and effects' part of a save, and crimes under way (game_save.cpp).
    std::vector<std::byte> save_actors() const;
    void load_actors(std::span<const std::byte> bytes);
    NavMeshes& navmeshes();
    Ai& ai();
    int race_size(uint32_t npc) const; // RACE DATA: 0 small to 3 extra large
    uint32_t race_flags(uint32_t npc); // RACE DATA flags
    std::string door_destination_name(uint32_t ref) const;
    // Once per corpse: its NPC_'s container, the death item (NPC_ INAM) and what it wears and holds.
    void stock_corpse(uint32_t ref, uint32_t inventory_npc, const std::vector<uint32_t>& worn);
    bool allows_dialogue(uint32_t ref, uint32_t npc);
    // The race's flight radius in units, when it flies (RACE DATA flag 0x80).
    std::optional<float> flight_radius(uint32_t npc) const;
    // An NPC_ with Keyword - Horse, or any flier with `tamed`: the Keyword - Mount of a quest alias, checked by the caller.
    std::optional<MountData> mount_data(uint32_t npc, bool tamed = false) const;
    bool race_has_keyword(uint32_t race, std::string_view keyword_id) const;
    uint32_t last_horse() const { return m_last_horse; }
    uint32_t ridden_horse = 0; // what the player is on now, for scripts
    bool dismount_asked = false; // Actor.Dismount
    void set_last_horse(uint32_t ref) { m_last_horse = ref; }
    // What a base is to a sandboxing actor.
    FurnitureKind furniture_kind(uint32_t base);
    // What the idle tree plays for furniture that isn't a chair to sit on: leaning, chopping, a ledge. Empty for a chair.
    const FurnitureEvents& furniture_events(uint32_t base);
    bool furniture_bench(uint32_t base) const; // WBDT bench type: a crafting station
    bool furniture_has_keyword(uint32_t base, std::string_view keyword_id) const;
    // The space a cell belongs to: its WRLD, or the interior CELL itself.
    uint32_t space_of_cell(uint32_t cell) const;

    // A keyword by EditorID, 0 when the load order has none.
    uint32_t keyword(std::string_view editor_id) const;
    // The gold value of a form: its own, or a potion's magic cost.
    int32_t base_value(const Item& item) const;
    // The value of an entry with its tempering and enchantment.
    int32_t value(const Stack& stack) const;
    // The name a list shows: the player's, or the form's, with the tempering prefix.
    std::string display_name(const Stack& stack) const;
    // Whether an entry shows in a list: not gold, nor anything nameless or unplayable.
    bool listed(const Stack& stack) const;
    // What the player does with an item, as shown on the card ().
    float weapon_damage(const Stack& stack) const;
    float armor_rating(const Stack& stack) const;
    float item_warmth(const Item& item) const; // Survival: what one piece gives
    float slot_warmth(const Item& item) const; // and what's worn in its slots
    // The bottom bar's totals.
    int32_t total_damage() const;
    float total_armor() const;
    // The card's effects text (the game's effects member).
    std::string effects_text(const Stack& stack) const;
    // A taught spell's card line and skill for a spell tome.
    std::string spell_text(uint32_t spell) const;
    int spell_skill(uint32_t spell) const;
    // The armor the player wears in an item's slots, for the card's change.
    float slot_rating(const Item& item) const;
    // A potion card's art: 0 Health, 1 Magicka, 2 anything else.
    int potion_type(const Item& item) const;
    std::string soul_name(uint8_t soul) const;
    // The list flag of an entry: its category, | 1 when favorited.
    uint32_t filter_flag(const Stack& stack) const;
    std::string skill_name(int actor_value) const;
    // An NPC_'s skill as its record gives it (the class's and levels' part is Actors).
    float npc_skill(uint32_t npc, int actor_value) const;
    float skill_percent(int actor_value) const; // towards the next level
    // What the player carries and can carry: actor values 31 and 32.
    float encumbrance() const;
    float carry_weight() const;
    // A record's CTDA list on the player, for a recipe being tempered or not.
    bool recipe_passes(uint32_t record, uint32_t item, bool enchanted);
    bool over_encumbered() const { return encumbrance() > carry_weight(); }

    // Perk entry points: `value` as the player's perks leave it; `args` follow the player (items, targets).
    float perk_value(int entry_point, float value, std::initializer_list<uint32_t> args = {}) const {
        return m_character->entry_point(entry_point, value, args);
    }
    // The player uses a skill: experience towards its next level.
    void use_skill(int actor_value, float amount, uint32_t object = 0, int action = -1) {
        m_character->use_skill(actor_value, amount, object, action);
    }
    // A skill goes up (books, training).
    void increment_skill(int actor_value, int levels) { m_character->increment_skill(actor_value, levels); }

    // equip.cpp. The Inventory Menu's selection of a stack: equips, takes off, eats or drinks; `hand` 0 right, 1 left,
    // -1 the item's default.
    ItemUse select(size_t stack, int hand);
    void equip_spell(uint32_t form, int hand);
    ItemUse poison_weapon(size_t stack);
    // The player reads a book: what it teaches. True when a spell tome was used up.
    bool read_book(const Item& book, uint32_t ref = 0); // ref: the copy lying in the world, which hears OnRead
    // The sound of picking up or putting down, or using (a potion's own): a SNDR EditorID.
    std::string item_sound(const Item& item, bool pickup, bool use) const;
    void notify(std::string text, std::string sound = {});
    void sound(std::string descriptor) { notify({}, std::move(descriptor)); }
    // ... unless the same text still waits to be shown.
    void notify_once(const std::string& text);
    std::vector<Notice> take_notices();

    // Enable state: its own, and its enable parent's (XESP), which can be the opposite.
    bool ref_disabled(uint32_t ref) const;
    bool set_ref_disabled(uint32_t ref, bool disabled); // false: it has an enable parent, which says
    // References whose state differs from their record, for what's loaded later; `on_ref_state` for what's there now.
    const std::unordered_map<uint32_t, bool>& ref_overrides() const { return m_ref_overrides; }
    std::function<void(uint32_t ref, bool disabled)> on_ref_state;
    void block_activation(uint32_t ref, bool blocked);
    bool activation_blocked(uint32_t ref) const { return m_blocked.contains(ref); }

    // Equipping for scripts: the first stack of a form, on or off.
    bool equip_form(uint32_t form, bool on);
    void unequip_all();
    bool is_equipped(uint32_t form) const;
    uint32_t equipped_weapon(bool left) const;
    // GetEquippedItemType +0x... : 0 nothing, 1 to 8 the melee kinds, 7 bow, 9 spell, 10 shield, 11 torch, 12 crossbow.
    int equipped_item_type(bool left) const;
    // Script-side hooks: the player reads a book (OnRead), equips an object (OnObjectEquipped).
    std::function<void(uint32_t form, uint32_t ref)> on_read;
    std::function<void(uint32_t form)> on_equipped, on_unequipped;
    // An event for a reference's scripts, its aliases' and the effects on it: forms by script type, then a number if it takes one.
    struct ScriptArg {
        uint32_t form = 0;
        const char* type = "ObjectReference";
    };
    std::function<void(uint32_t ref, std::string_view event, std::vector<ScriptArg> forms, std::optional<int32_t> number)> on_script_event;
    void script_event(uint32_t ref, std::string_view event, std::vector<ScriptArg> forms = {}, std::optional<int32_t> number = {}) {
        if (on_script_event) {
            on_script_event(ref, event, std::move(forms), number);
        }
    }

    // An item changed hands: to 0 it left the world's containers, from 0 it's new. For the scripts.
    std::function<void(uint32_t form, int32_t count, uint32_t from, uint32_t to)> on_item_moved;
    void inventory_changed(uint32_t owner, uint32_t form, int32_t delta) override;
    void flush_item_moves(); // what left and arrived nowhere, once a frame
    int32_t count_of(const Inventory& inventory, uint32_t form) const; // a form or a FLST's forms

    // Default object 328, Survival_ModeToggle: what "survival on" means to the game.
    bool survival() const;
    int32_t stat(const std::string& name) const;
    void increment_stat(const std::string& name, int32_t by = 1);
    float warmth() const;
    bool in_region(uint32_t region) const;
    Leveled& leveled() { return m_leveled; }
    // What scripts place: a new reference of a base, at a spot in Skyrim units; and what they delete.
    uint32_t place_form(uint32_t base, const std::array<float, 3>& position);
    uint32_t placed_base(uint32_t ref) const {
        const auto it = m_placed_forms.find(ref);
        return it == m_placed_forms.end() ? 0 : it->second;
    }
    void forget_ref(uint32_t ref) {
        m_moved.erase(ref);
        m_placed_forms.erase(ref);
    }
    // A reference scripts moved: where it is now, in Skyrim units.
    void move_ref(uint32_t ref, const std::array<float, 3>& position) { m_moved[ref] = position; }
    std::optional<std::array<float, 3>> ref_position(uint32_t ref) const;
    std::array<float, 3> ref_angle(uint32_t ref) const; // radians: what scripts set, then the placement's
    float ref_scale(uint32_t ref) const;
    // Back to where the record has it.
    void restore_ref_place(uint32_t ref) {
        m_moved.erase(ref);
        if (const auto it = m_changes.find(ref); it != m_changes.end()) {
            it->second.angle.reset();
        }
    }
    const std::array<float, 3>* moved_position(uint32_t ref) const {
        const auto it = m_moved.find(ref);
        return it == m_moved.end() ? nullptr : &it->second;
    }

    // The OBND of what the player can act on (x1 y1 z1 x2 y2 z2, Skyrim units), null for anything else.
    const std::array<float, 6>* activatable(uint32_t base);
    // What a reference is to the player; kind none for anything they can't act on, and for what's gone.
    Target inspect(uint32_t ref);
    // What a live actor is to the player: Talk, or Pickpocket when sneaking.
    Target inspect_actor(uint32_t ref, uint32_t npc, bool sneaking);
    // The player's, or a container's.
    Inventory& inventory_of(uint32_t ref) { return ref == k_player_ref ? m_player.inventory : container(ref); }
    // The contents of a container reference or an NPC_, made from its record on first use.
    Inventory& container(uint32_t ref);
    // A save's change layer (docs/formats/ost.md): game_save.cpp.
    std::vector<std::byte> save() const;
    void load(std::span<const std::byte> bytes);
    // What one of a form or levelled list gives at the player's level.
    std::vector<Rolled> roll(uint32_t form) const;
    // An actor reset: their inventory and actor values as the records have them.
    void reset_ref(uint32_t ref) { // ObjectReference.Reset: back where the world placed it
        m_moved.erase(ref);
        reset_actor(ref);
    }
    void reset_actor(uint32_t ref) {
        m_containers.erase(ref);
        m_actor_values.erase(ref);
        script_event(ref, "OnReset");
    }
    // A cell's reset, for what the player has changed in it: containers with the
    // Respawns flag fill again, plants grow back, what was taken returns unless it is persistent or No Respawn.
    void reset_cell(uint32_t cell);
    // Empties it, to be made again (a vendor's chest, a respawning container).
    void reset_container(uint32_t ref) { m_containers.erase(ref); }
    // as far as records tell: the reference's XOWN, else its cell's.
    uint32_t owner_of(uint32_t ref) const;
    uint32_t own_owner(uint32_t ref) const; // the reference's or cell's own the game, no fallback
    // Whether the player may take from an owner: their own, a faction they belong to, or a friend's up to
    // the friendship's favor value.
    bool may_take(uint32_t owner, int32_t value) const;
    // The crime faction of where a reference is: its cell's location's, or a parent location's.
    uint32_t location_crime_faction(uint32_t ref) const;
    // The rank the records give two NPC_s (RELA): -4 archnemesis .. 4 lover.
    int record_relationship(uint32_t a, uint32_t b) const;
    // Whether a RELA of that association type joins two NPC_s, or `a` to anyone with `b` 0.
    bool has_association(uint32_t a, uint32_t type, uint32_t b) const;
    // A placed reference's base; the player's reference counts as NPC_ 7.
    uint32_t base_of(uint32_t ref) const;
    // Factions the player joined or left at run time, on the record's.
    void set_player_faction(uint32_t faction, bool member);
    bool in_player_faction(uint32_t faction) const { return m_player_factions.contains(faction); }
    // What scripts changed on a reference, over its record.
    RefChanges& changes(uint32_t ref) { return m_changes[ref]; }
    // References whose rank in that faction, or membership of any, scripts have changed.
    std::vector<uint32_t> refs_changing_faction(uint32_t faction) const {
        std::vector<uint32_t> out;
        for (const auto& [ref, c] : m_changes) {
            if (c.left_all_factions || c.factions.contains(faction)) {
                out.push_back(ref);
            }
        }
        return out;
    }
    const RefChanges* find_changes(uint32_t ref) const {
        const auto it = m_changes.find(ref);
        return it == m_changes.end() ? nullptr : &it->second;
    }
    bool ref_flag(uint32_t ref, RefFlag flag) const {
        const RefChanges* c = find_changes(ref);
        return c && (c->flags & flag) != 0;
    }
    void set_ref_flag(uint32_t ref, RefFlag flag, bool on) {
        uint32_t& flags = changes(ref).flags;
        flags = on ? flags | flag : flags & ~uint32_t(flag);
    }
    // Set by the app: what only the world knows.
    std::function<bool(uint32_t ref)> ref_dead, ref_loaded;
    std::function<uint32_t()> dialogue_speaker;
    // The sit and sleep states, as GetSitState and GetSleepState give them: 2 on the way, 3 there, 4 getting up.
    int32_t sit_state(uint32_t ref);
    void unequip_where(const std::function<bool(const Item&, const Stack&)>& test);
    void set_player_sit_state(int32_t state) { m_player_sit = state; }
    void set_player_furniture(uint32_t ref) { m_player_furniture = ref; }
    uint32_t player_furniture() const { return m_player_furniture; }
    int32_t sleep_state(uint32_t ref);
    bool is_ghost(uint32_t ref) const;
    void set_ghost(uint32_t ref, bool ghost);
    // The one actor doing the player a favor (Actor.SetDoingFavor), 0 for none.
    uint32_t favor_actor() const { return m_favor_actor; }
    void set_favor_actor(uint32_t ref) { m_favor_actor = ref; }
    // An NPC_'s outfits as scripts left them, 0 for the record's; `sleep` the sleep outfit.
    uint32_t base_outfit(uint32_t npc, bool sleep) const {
        const auto it = m_base_outfits.find(npc);
        return it == m_base_outfits.end() ? 0 : sleep ? it->second.second : it->second.first;
    }
    void set_base_outfit(uint32_t npc, bool sleep, uint32_t outfit) {
        auto& o = m_base_outfits[npc];
        (sleep ? o.second : o.first) = outfit;
        if (!sleep && on_outfit) {
            on_outfit(npc, outfit);
        }
    }
    std::function<void(uint32_t npc, uint32_t outfit)> on_outfit;
    // Game.TeachWord: the word shows as learnt; Casters holds the unlocked.
    void teach_word(uint32_t word) { m_known_words.insert(word); }
    // Deaths by NPC_, for GetDeadCount.
    void count_death(uint32_t npc) { ++m_dead_counts[npc]; }
    int32_t dead_count(uint32_t npc) const {
        const auto it = m_dead_counts.find(npc);
        return it == m_dead_counts.end() ? 0 : it->second;
    }
    // Location.SetKeywordData's values.
    float location_data(uint32_t location, uint32_t keyword) const {
        const auto it = m_location_data.find({location, keyword});
        return it == m_location_data.end() ? 0.0f : it->second;
    }
    void set_location_data(uint32_t location, uint32_t keyword, float value) { m_location_data[{location, keyword}] = value; }
    // An actor's rank in a faction, k_not_in_faction for none: what scripts set, then its NPC_'s SNAM.
    int32_t faction_rank(uint32_t ref, uint32_t faction);
    void set_faction_rank(uint32_t ref, uint32_t faction, int32_t rank);
    void leave_faction(uint32_t ref, uint32_t faction);
    void leave_all_factions(uint32_t ref);
    // Its crime faction: what scripts set, then the record's.
    uint32_t crime_faction(uint32_t ref) const;
    // FURN: MNAM's marker bits, and FNPR's entry points of a marker (ENAM gives each its number; 0: none listed).
    // activating a pile is activating the body it was left by.
    void link_ash_pile(uint32_t pile, uint32_t actor) { m_ash_piles[pile] = actor; }
    uint32_t ash_pile_actor(uint32_t pile) const {
        const auto it = m_ash_piles.find(pile);
        return it == m_ash_piles.end() ? 0 : it->second;
    }
    uint32_t furniture_active_markers(uint32_t furniture) const;
    uint16_t furniture_entry_points(uint32_t furniture, uint32_t marker) const;
    // ACBS flags scripts set on an NPC_, which every reference to it shares.
    bool base_flag(uint32_t npc, uint32_t flag) const;
    void set_base_flag(uint32_t npc, uint32_t flag, bool on);
    // The player harvests a plant.
    bool harvest(const Target& target);
    bool harvested(uint32_t ref) const { return m_harvested.contains(ref); }
    // Whose lock a reference answers to: its own, else that of the door it leads to.
    uint32_t lock_holder(uint32_t ref) const;
    void unlock(uint32_t ref) {
        if (m_unlocked.insert(lock_holder(ref)).second) {
            script_event(lock_holder(ref), "OnLockStateChanged");
        }
    }
    void relock(uint32_t ref) {
        if (m_unlocked.erase(lock_holder(ref))) {
            script_event(lock_holder(ref), "OnLockStateChanged");
        }
    }
    // The name of a lock level, 0 Novice .. 5 Requires Key, from the XLOC value.
    int lock_index(uint8_t level) const;
    LockNumbers lock_numbers(int index) const;
    float pick_health = 100; // g_LockpickingPickHealth: carried over between locks

    // A vendor: the faction that makes an NPC_ one (0 for none), its chest and lists.
    struct Vendor {
        uint32_t faction = 0, chest = 0, list = 0;
        bool buys_stolen = false, not_list = false;
    };
    Vendor vendor(uint32_t npc) const;
    // The chest's respawn when the barter menu opens: every iDaysToRespawnVendor days.
    void refresh_vendor(const Vendor& vendor);
    // Whether a vendor deals in an item.
    bool vendor_accepts(const Vendor& vendor, const Item& item) const;
    float buy_mult(uint32_t partner = 0) const;
    float sell_mult(uint32_t partner = 0) const;
    double days_passed = 0; // game days, kept by the app
    bool taken(uint32_t ref) const { return m_taken.contains(ref); }
    // A dropped stack lying in the world: its reference's id, and those near a point.
    struct Placed {
        Stack stack;
        std::array<float, 3> position; // metres
    };
    uint32_t place(const Stack& stack, const std::array<float, 3>& position);
    const std::unordered_map<uint32_t, Placed>& placed() const { return m_placed; }
    void remove_placed(uint32_t ref) { m_placed.erase(ref); }
    // The player picks an item reference up; true when it left the world.
    bool take(const Target& target);

private:
    std::string effect_lines(std::span<const Effect> effects) const;
    void equip_hand(Stack& stack, const Item& item, int hand);
    void eat_ingredient(const Item& item);
    void ensure_ammo(const Item& weapon);

    std::unique_ptr<forms::Database> m_db;
    Settings m_settings;
    Items m_items;
    Magic m_magic;
    WorldState m_world;
    VatsEntry m_vats;
    DetectionEvent m_detection_event;
    Conditions m_conditions;
    CameraPaths m_camera_paths;
    std::unique_ptr<Character> m_character;
    Player m_player;
    Crafting m_crafting{*this};
    Crime m_crime{*this};
    Combat m_combat{*this};
    CombatAi m_combat_ai{*this};
    Idles m_idles{*m_db, m_conditions};
    Projectiles m_projectiles{*this};
    Impacts m_impacts{*this};
    std::unique_ptr<NavMeshes> m_navmeshes;
    std::unique_ptr<Ai> m_ai;
    std::unordered_map<uint32_t, FurnitureKind> m_furniture_kinds;
    std::unordered_map<uint32_t, std::array<float, 3>> m_body_halves;
    std::unordered_map<uint32_t, FurnitureEvents> m_furniture_events;
    struct IdleRoot {
        int64_t function = 0, param = 0; // HasKeyword or GetIsID, on the furniture
        FurnitureEvents events;
    };
    std::vector<IdleRoot> m_idle_roots;
    bool m_idle_roots_read = false;
    int32_t m_player_sit = 0; // GetSitState's numbers
    uint32_t m_player_furniture = 0;
    uint32_t m_last_horse = 0;
    Leveled m_leveled;
    std::unordered_map<uint32_t, Inventory> m_containers;
    std::unordered_map<uint32_t, std::array<float, 6>> m_bounds; // of bases seen; all zero: not activatable
    std::unordered_set<uint32_t> m_taken;
    mutable std::unordered_map<std::string, uint32_t> m_keywords;
    std::vector<Notice> m_notices;
    std::unordered_map<uint32_t, ActorValues> m_actor_values;
    std::unordered_map<uint32_t, std::unique_ptr<MagicTarget>> m_magic_targets;
    std::vector<MagicTarget*> m_magic_order; // by index: an effect can bring in new targets
    MagicWorld m_no_magic_world;
    MagicWorld* m_magic_world = &m_no_magic_world;
    Casters m_casters{*this};
    uint32_t m_next_effect = 1;
    std::unordered_map<std::string, int32_t> m_stats;
    QuestSystem* m_quest_system = nullptr;
    std::unordered_map<uint32_t, uint32_t> m_ash_piles;
    struct Leaving {
        uint32_t form = 0;
        int32_t count = 0;
        uint32_t from = 0;
    };
    std::vector<Leaving> m_leaving;
    std::unordered_map<uint32_t, std::array<float, 3>> m_moved;
    std::unordered_map<uint32_t, uint32_t> m_placed_forms; // reference: its base
    std::unordered_map<uint32_t, int> m_worn_effects; // enchantment: how many worn pieces carry it
    mutable std::unordered_map<uint64_t, int> m_relationships; // RELA, made on first use
    std::unordered_set<uint32_t> m_harvested, m_unlocked, m_blocked;
    struct EnableChild {
        uint32_t ref = 0;
        bool opposite = false;
    };
    void read_enable_parents() const;
    void publish_ref_state(uint32_t ref);
    mutable bool m_read_parents = false;
    mutable std::unordered_map<uint32_t, std::vector<EnableChild>> m_enable_children;
    mutable std::unordered_map<uint32_t, EnableChild> m_enable_parent; // child: its parent
    std::unordered_map<uint32_t, bool> m_own_disabled;   // what scripts set
    std::unordered_map<uint32_t, bool> m_ref_overrides;  // the result, where it differs from the record
    std::unordered_set<uint32_t> m_player_factions;
    std::unordered_map<uint32_t, RefChanges> m_changes;
    std::unordered_map<uint32_t, int32_t> m_dead_counts;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> m_base_outfits; // NPC_: default, sleep
    uint32_t m_favor_actor = 0;
    std::unordered_set<uint32_t> m_known_words;
    std::map<std::pair<uint32_t, uint32_t>, float> m_location_data;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> m_base_flags; // NPC_: bits set, bits cleared
    std::unordered_map<uint32_t, double> m_vendor_stamps;
    std::unordered_map<uint32_t, Placed> m_placed;
    uint32_t m_next_placed = 0xFF000800; // the game's created references start in the FF range
    uint32_t m_tempering_item = 0; // the condition list of a recipe reads it
    bool m_tempering_enchanted = false;
};

} // namespace spud::game
