#pragma once

// NPC AI: which package each actor runs, where that puts them, and walking there.

#include "game/conditions.hpp"
#include "game/conversations.hpp"
#include "game/navmesh.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <optional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spud::game {

class Game;

enum class AiMode : uint8_t { stay, travel, sandbox, sleep, patrol, follow, force_greet, orbit, hover, sit };

// Where a package wants its actor.
struct AiLocation {
    uint32_t space = 0; // WRLD, or an interior's CELL; 0 for wherever they are
    NavPoint point{};
    float radius = 0;
    bool valid() const { return space != 0; }
};

// PACK as selection reads it.
struct AiPackage {
    uint32_t form = 0;
    uint32_t flags = 0;
    uint16_t interrupts = 0; // PKDT interrupt flags: 2 random conversations
    uint8_t preferred_speed = 0;
    int8_t month = -1, day_of_week = -1, date = 0, hour = -1, minute = -1;
    uint32_t duration_minutes = 0;
    std::vector<ConditionItem> conditions; // the template's too
    AiMode mode = AiMode::stay;
    int32_t location_type = -1;   // PLDT
    uint32_t location_form = 0;
    float location_radius = 0;
    uint32_t topic = 0;  // PDTO type 0: what a ForceGreet opens with
    uint32_t target = 0; // PTDA a specific reference
    bool target_linked = false; // PTDA the actor's linked reference
    bool idle_marker = false;   // UseIdleMarker: the sit mode, on a marker
    bool patrol_repeats = true;
    bool ride_horse = false;    // "Ride Horse if Possible?"
    std::vector<float> floats; // the data inputs' values: an orbit's radii and height
    // A sandbox's inputs, as the game's bits: 1 eating, 2 sleeping, 8 idle markers, 0x10 sitting, 0x20 wandering.
    uint8_t allow = 0x3B;
    float energy = -1; // the input; below 0, the actor's own
    bool default_master = false;
    bool follower = false; // FollowerPackageTemplate: WaitingForPlayer holds them where they are
};

struct AiClock {
    float hour = 12, time_scale = 20;
    int month = 7, date = 17, day_of_week = 0;
};

// actorState1's fly state.
enum class FlyState : uint8_t { none, take_off, cruising, hovering, landing, perching, action };
struct AiLanding {
    NavPoint position{};
    bool crash = false;
};
struct Flight {
    std::vector<NavPoint> line; // the curve, sampled
    size_t at = 0;
    float along = 0;
    float timer = 0;
    NavPoint goal{};
    bool leaving = false;       // on the way down to `goal`
    AiMode order = AiMode::stay; // from combat, scripts or tests, over the package: orbit, hover, or travel to land
    NavPoint centre{};
};

// What a sandboxing actor can use.
enum class FurnitureKind : uint8_t { none, chair, bed, bedroll, idle_marker, work }; // work: the player's, by the idle tree
// One place to use: a furniture's marker, its reference's own place when the model has none.
struct AiFurniture {
    uint32_t ref = 0, base = 0;
    FurnitureKind kind = FurnitureKind::none;
    NavPoint position{};
    float heading = 0;
    uint8_t marker = 0;
    uint16_t entry = 0; // 1 front, 2 behind, 4 right, 8 left, 16 up; 0 when the record says nothing
    uint64_t key() const { return uint64_t(ref) << 8 | marker; }
};

// The idle tree's events for a piece of furniture.
struct FurnitureEvents {
    std::string enter, instant;
};

// The sandbox's activities, by the game's numbers.
enum class Activity : int8_t { none = -1, sit, sleep, eat_standing, eat_sitting, wander, idle_marker };
enum class UseState : uint8_t { none, going, using_it, leaving };

// References made at run time start here.
inline constexpr uint32_t k_created_refs = 0xFF000000;

struct Agent {
    uint32_t ref = 0, npc = 0;
    uint32_t space = 0;
    NavPoint position{};
    float heading = 0, scale = 1;
    uint32_t home_space = 0; // the editor location
    NavPoint home{};
    const AiPackage* package = nullptr;
    AiLocation location;
    bool high = false; // the player's attached cells: this one walks
    int last_hour = -1;
    float evaluate_in = 0;
    // Walking.
    std::vector<NavPoint> path;
    size_t next = 0;
    NavSpot spot;
    bool searching = false;
    double no_path_until = 0;
    uint32_t leg_door = 0; // the door this path leads to, on the way to another space
    float door_eta = 0;    // a follower out of sight: seconds until it gets there
    float rest = 0;        // seconds until a sandboxing actor moves on
    float wander_wait = 0; // wandering: seconds until the next leg, once the last has ended
    int slot = -1;         // the daily slot rolled: 0 sleep, 1 breakfast, 2 lunch, 3 dinner
    float slot_start = 0, slot_hours = 0;
    bool slot_entered = false;
    float eat_in = -1;     // eating sitting: seconds until the meal starts, once seated
    float speed = 0;
    bool script_placed = false; // moved to where it isn't high yet: its package doesn't send it straight back
    uint32_t patrol = 0;   // the marker a patrol is walking to
    uint32_t rides = 0;    // the actor's horse (XHOR), for a package that rides
    uint32_t follows = 0;  // the reference a Follow goes after
    uint32_t commander = 0; // a summon or raised corpse: it keeps by them, and packages wait
    float fall_speed = -1;  // a flier killed in the air, units a second; below 0 when not falling
    float crash_left = -1;  // killed in the air: how long it flies on; below 0 otherwise
    bool crashing = false;  // falling with a kill to come when it lands
    NavPoint start{};      // where the package started: "near package start"
    bool disabled = false;
    uint32_t detach_hours = 0; // Actor: the hours they last left high process, 0 for never
    // What they know of the player.
    int detection = -1000; // above 0: seen
    bool los = false;
    float greeting_timer = 0, chatter_timer = 0;
    bool saying = false;   // stopped for a line
    bool faces_player = false; // turning to them at the movement type's rate
    bool arresting = false; // a guard coming for the player
    bool fighting = false;  // combat has it: its packages wait
    float stop_distance = 128; // how near a follower or fighter comes
    // Sandboxing.
    Activity activity = Activity::none;
    UseState use = UseState::none;
    uint32_t use_ref = 0;      // the furniture or marker
    uint8_t use_marker = 0;
    float leaving = 0;         // seconds until up from it
    std::array<uint8_t, 6> since{}; // changes since each activity was chosen
    bool arrived_unseen = false;   // came into the attached cells: already at what they're doing
    std::string event;         // an animation event for the app to send
    // Talking to another NPC.
    float talk_timer = -1;        // high, rolled when they first come into the attached cells
    uint32_t talk_event = 0;      // high: the ADIA waiting for its answer
    bool in_random_scene = false; // & 0x10
    bool talk_initiator = false;  // high
    uint32_t talk_partner = 0;    // high, once the scene brings them together
    float talk_stop = -1;         // the speaker's stop distance; below 0 for the one spoken to
    std::vector<uint32_t> talked_to; // high: not asked again until the timer clears it
    float talked_timer = 0;          // high
    // Flying.
    bool flier = false, allow_flying = true, resolved = false;
    uint32_t landing_marker = 0; // SetForcedLandingMarker: the next landing, whatever else is nearer
    AiLanding forced_landing;
    FlyState fly = FlyState::none;
    float flight_radius = 400, pitch = 0;
    Flight flight;
    std::string idle; // an IDLE for the app to play
    // Stuck handling.
    NavPoint window_from{};
    float window_time = 0, wanted = 0;
    int failures = 0;
};

class Ai : public Conversations {
public:
    explicit Ai(Game& game);
    ~Ai();

    void add(uint32_t ref, uint32_t npc, uint32_t space, const NavPoint& position, float heading, float scale);
    // Each frame. `high` are the actors the player's attached cells hold.
    void update(float dt, const AiClock& clock, const std::vector<uint32_t>& high);
    // Who is in a space now; moves when somebody leaves or arrives.
    std::vector<const Agent*> in_space(uint32_t space) const;
    uint32_t roster() const { return m_roster; }
    size_t searches() const { return m_searches.size(); }
    void roster_changed() { ++m_roster; } // an actor was enabled or disabled
    const Agent* agent(uint32_t ref) const;
    bool furniture_in_use(uint32_t ref) const; // any of its markers taken
    const AiFurniture* free_seat(uint32_t space, uint32_t ref, const NavPoint& near) const; // the nearest marker nobody has
    // Packages run for this actor at once (the EvaluatePackage native, an alias filled).
    void evaluate(uint32_t ref);
    size_t agents() const { return m_agents.size(); }
    // Those following a reference, wherever they are.
    std::vector<uint32_t> followers(uint32_t leader) const;
    // Moved by the game, not by walking: fast travel, a load.
    void place_at(uint32_t ref, uint32_t space, const NavPoint& position, float heading);
    void set_disabled(uint32_t ref, bool disabled); // Enable, Disable: out of the world until enabled
    // A save's actors: those away from their record, and the created ones (game_save.cpp). The roster may still be coming
    // when a save loads, so the bytes wait for apply_saved.
    void save(BinaryWriter& w) const;
    void set_saved(std::vector<std::byte> bytes) { m_saved = std::move(bytes); }
    void apply_saved();
    // Everyone, for what the world and saves ask of them all.
    template <class F>
    void for_each(F&& f) const {
        for (const auto& [ref, a] : m_agents) {
            f(a);
        }
    }
    // What everyone is doing, as one number: two runs that replayed identically have the same one.
    uint64_t digest() const;
    // A run that must replay: searches land a fixed number of updates after they start, not when their thread is done.
    void set_replay(bool on) { m_replay = on; }
    void dump(const std::string& file) const; // the same, one line each, sorted: for diffing two runs
    // A guard goes after the player, or stops.
    void pursue(Agent& a, bool on);
    float walk_speed() const { return m_walk_speed; }
    float run_speed() const { return m_run_speed; }

    // A fight moves an actor: towards a target to `stop` units, away from it, or back to its packages (combat_ai.cpp).
    void close_on(uint32_t ref, uint32_t target, float stop);
    void flee(uint32_t ref, uint32_t from, int attempt = 0);
    void stand_down(uint32_t ref);
    void command(uint32_t ref, uint32_t commander);
    void drop(uint32_t ref); // a dead flier: down to the ground
    bool crash_land(uint32_t ref); // false when it isn't in the air; it lands, and the kill goes through there
    // Up from what they are doing: a chair or bed by its exit, an idle by the graph's default state.
    void stand_up(uint32_t ref);
    // Scenes and dialogue hold an actor where they are.
    void hold(uint32_t ref, bool held);
    Agent* mutable_agent(uint32_t ref);
    // Where the player is, for those who go after them.
    void set_furniture(uint32_t space, std::vector<AiFurniture> items);
    void set_landings(uint32_t space, std::vector<AiLanding> places) { m_landings[space] = std::move(places); }
    // What the graph says of a flier, and an order to it.
    void flight_event(uint32_t ref, std::string_view tag);
    void order_flight(uint32_t ref, AiMode mode, const NavPoint& centre);
    // The land under a point in the player's space, NaN when unknown.
    std::function<float(float x, float y)> ground_height;
    void set_player(uint32_t space, const NavPoint& position, std::vector<uint32_t> ground = {}) {
        m_player_space = space;
        m_player = position;
        m_player_ground = std::move(ground);
    }
    void set_player_running(bool running) { m_player_running = running; }
    // Quest aliases' packages, first in line. Set by the app once quests run.
    std::function<std::vector<uint32_t>(uint32_t ref)> alias_packages;
    // The ADIA story event, its id; 0 while the player can't have one. And whether a scene has an actor.
    std::function<uint32_t(uint32_t a, uint32_t b)> ask_conversation;
    std::function<bool(uint32_t ref)> in_scene;
    // A test's package, over the others.
    void force_package(uint32_t ref, uint32_t form) {
        m_forced[ref] = form;
        evaluate(ref);
    }
    std::function<bool(uint32_t from, uint32_t to)> line_of_sight;

    void event_processed(uint32_t event, bool started) override;
    bool can_talk(uint32_t a, uint32_t b) override;
    uint32_t initiator(uint32_t a, uint32_t b) override;
    void start_talk(uint32_t speaker, uint32_t target) override;
    bool talk_ready(uint32_t speaker) override;
    bool talk_broken(uint32_t a, uint32_t b) override;
    void end_talk(uint32_t a, uint32_t b) override;
    float setting(const char* name, float fallback) override;
    std::vector<std::byte> m_saved;

private:
    const AiPackage* package(uint32_t form);
    bool qualifies(const Agent& a, const AiPackage& p, const AiClock& clock);
    const AiPackage* select(const Agent& a, const AiClock& clock);
    const std::vector<uint32_t>& packages_of(uint32_t npc);
    const std::vector<uint32_t>& defaults_of(uint32_t npc);
    AiLocation resolve(const Agent& a, const AiPackage& p);
    void think(Agent& a, const AiClock& clock);
    void start(Agent& a, const AiPackage* p);
    void place(Agent& a);
    void walk(Agent& a, float dt);
    void request_path(Agent& a, const NavPoint& to);
    void build_doors();
    bool reference_location(uint32_t ref, AiLocation& out);
    uint32_t linked_ref(uint32_t ref);
    float arrival_radius(uint32_t ref, float radius);
    void follow(Agent& a);
    void fly(Agent& a, float dt);
    bool wants_air(const Agent& a) const;
    float floor_under(const NavPoint& p, float radius) const;
    std::vector<NavPoint> fly_path(const Agent& a, const NavPoint& goal) const;
    void fly_along(Agent& a, const std::vector<NavPoint>& points);
    void start_orbit(Agent& a, const NavPoint& centre, float min_radius, float max_radius, float height);
    const AiLanding* landing_near(const Agent& a) const;
    void pick_activity(Agent& a, bool instant);
    void arrive_at_use(Agent& a, bool instant);
    void leave_use(Agent& a);
    float activity_seconds(const Agent& a, Activity activity);
    float energy_of(const Agent& a); // the sandbox's input, or the NPC's AIDT
    void wander_leg(Agent& a);
    const AiFurniture* sit_place(const Agent& a); // what a Sit or UseIdleMarker package goes to
    void update_sit(Agent& a, float dt, float away);
    bool may_use(const Agent& a, uint32_t ref);
    NavPoint steer(const Agent& a, float dx, float dy, float distance, float step);
    void check_stuck(Agent& a, float dt, float step);
    std::string idle_event(uint32_t idle_marker_base);
    uint32_t door_towards(uint32_t from, uint32_t to) const;
    void change_space(Agent& a, uint32_t space, const NavPoint& position, float heading);

    struct Door {
        uint32_t ref = 0, destination = 0;
        uint32_t space = 0, destination_space = 0;
        NavPoint position{}, arrival{};
        float arrival_heading = 0;
    };

    Game& m_game;
    std::unordered_map<uint32_t, std::unique_ptr<AiPackage>> m_packages;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_lists, m_default_lists;
    std::unordered_map<uint32_t, float> m_energy; // AIDT energy by NPC
    std::unordered_map<uint32_t, Agent> m_agents;
    std::vector<uint32_t> m_order; // round robin over those not in the attached cells
    size_t m_cursor = 0;
    std::unordered_set<uint32_t> m_high, m_held;
    uint32_t m_roster = 0;
    mutable uint32_t m_by_space_roster = ~0u; // who is where, as of that roster
    mutable std::unordered_map<uint32_t, std::vector<const Agent*>> m_by_space;
    int m_hour = -1;
    float m_clock_hour = 12;
    void roll_slot(Agent& a);
    void update_conversations(float dt, const std::vector<uint32_t>& high);
    void update_left_behind(float dt);
    float m_left_behind_check = 0;
    void close_talk(Agent& a, Agent& b);
    bool may_talk(const Agent& subject, const Agent& other);
    bool can_be_talked_to(const Agent& other, const Agent& subject);
    bool can_speak(uint32_t npc);
    float roll_talk_timer() const;
    std::unordered_map<uint32_t, bool> m_can_speak; // by NPC_
    void set_doors_locked(const Agent& a, bool locked);
    float m_walk_speed = 0, m_run_speed = 0, m_turn_speed = 6.28f, m_time_scale = 20;
    uint32_t m_player_space = 0;
    std::vector<uint32_t> m_player_ground; // that space and the merged cities in it
    bool m_player_running = false;
    std::unordered_map<uint32_t, uint32_t> m_forced;
    NavPoint m_player{};
    std::unordered_map<uint32_t, std::vector<AiFurniture>> m_furniture; // by space
    std::unordered_map<uint64_t, uint32_t> m_reserved;                  // AiFurniture::key -> agent
    std::unordered_map<uint32_t, std::vector<AiLanding>> m_landings;    // by space
    float m_fly_walk_speed = 2000, m_fly_run_speed = 7400;
    std::unordered_map<uint32_t, Door> m_doors;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_doors_of_space;
    std::future<void> m_door_index;
    bool m_doors_ready = false;
    // Path searches run on their own thread.
    struct Search {
        uint32_t ref = 0;
        std::future<std::vector<NavPoint>> result;
        uint32_t ticks = 0; // updates since it was asked for
    };
    std::deque<Search> m_searches;
    // Agents new to the attached cells, being put on the ground by a worker.
    struct Arrival {
        uint32_t ref = 0, space = 0;
        NavPoint position{};
    };
    struct Grounding {
        std::vector<Arrival> spots;
        std::future<std::vector<std::optional<NavPoint>>> found;
        uint32_t ticks = 0;
    };
    std::vector<Grounding> m_groundings;
    double m_search_clock = 0;
    bool m_replay = false;
};

} // namespace spud::game
