#pragma once

// Scenes. What dialogue and packages must do
// comes through SceneActors (13); the default has nothing to say or run, so such an action ends at once and a scene's
// phases, timers and fragments still advance.

#include "game/quests.hpp"

#include <functional>
#include <limits>
#include <vector>

namespace spud::game {

class SceneActors {
public:
    virtual ~SceneActors() = default;
    virtual bool is_actor(uint32_t) { return false; }
    virtual bool is_dead(uint32_t) { return false; }
    virtual bool start_saying(uint32_t /*speaker*/, uint32_t /*topic*/, uint32_t /*target*/) { return false; }
    virtual bool is_saying(uint32_t) { return false; }
    virtual bool start_package(uint32_t /*actor*/, uint32_t /*package*/) { return false; }
    virtual bool package_done(uint32_t /*actor*/, uint32_t /*package*/) { return true; }
    virtual void clear_package(uint32_t /*actor*/) {}
    virtual uint32_t speaker() { return 0; }
    virtual bool conversation_active() { return false; }
};

enum SceneFlag : uint32_t {
    scene_begin_on_quest_start = 0x1, scene_stop_quest_on_end = 0x2, scene_repeat_while_true = 0x8, scene_interruptible = 0x10
};
enum SceneActionFlag : uint32_t { action_face_target = 0x8000, action_looping = 0x10000, action_head_track_player = 0x20000 };

class Scenes {
public:
    explicit Scenes(QuestSystem& quests);

    void set_actors(SceneActors* actors) { m_actors = actors; }
    // A dialogue action's look: the speaker at its head-track actor, or the player (target 0 lets go). The same look as
    // a script's SetLookAt.
    std::function<void(uint32_t actor, uint32_t target)> look_at;
    void update(float dt); // section 4
    void quest_started(Quest& q); // Begin on Quest Start
    void quest_stopped(Quest& q); // 8
    void alias_changed(Quest& q, uint32_t alias); // 5
    void ref_left(Quest& q, uint32_t ref);
    void bind_natives();

    bool is_playing(uint32_t scene) const;
    uint32_t scene_of(uint32_t ref); // the playing scene it's in, 0 for none
    bool has_actor(uint32_t ref) const; // a scene that has begun, or is bringing its pair together
    int current_phase(uint32_t scene) const;
    bool action_complete(uint32_t scene, uint32_t action) const;
    // A playing scene as text, for checks: its phase and each action of that phase, running or done.
    std::string describe(uint32_t scene) const;
    // Scene.Start and ForceStart; false when the quest isn't running.
    bool start(uint32_t scene);
    bool force_start(uint32_t scene);
    void stop(uint32_t scene);

private:
    struct Phase {
        std::vector<ConditionItem> start_conditions, completion_conditions;
        bool started = false, fragment_pending = false, fragment_done = false, ending = false;
    };
    struct Action {
        uint16_t type = 0; // 0 dialogue, 1 package, 2 timer
        int32_t alias = -1;
        uint32_t index = 0, flags = 0, start_phase = 0, end_phase = 0;
        uint32_t topic = 0;
        int32_t head_track = -1;
        float loop_min = 1.0f, loop_max = 10.0f;
        std::vector<uint32_t> packages;
        float duration = 0;
        // Runtime.
        bool running = false, complete = false, saying = false;
        float delay = 0, elapsed = 0;
        size_t package = 0;
        bool event_sent = false, fragment_done = false;
        bool looking = false; // its speaker's look was set
    };
    struct Actor {
        int32_t alias = -1;
        uint32_t flags = 0, behaviours = 0x1A;
    };
    struct Scene {
        uint32_t form = 0, quest = 0, flags = 0;
        std::vector<Phase> phases;
        std::vector<Action> actions;
        std::vector<Actor> actors;
        std::vector<ConditionItem> conditions;
        bool playing = false, hold = false, all_done = false, fragment_done = false, end_sent = false;
        bool random = false; // an ADIA quest's (9)
        uint32_t speaker = 0, spoken_to = 0;
        std::array<uint32_t, 2> pair{}; // flagged in a random scene until it stops
        int phase = -1;
        float end_delay = std::numeric_limits<float>::max();
    };

    void load_records();
    Scene* scene(uint32_t form);
    const Scene* scene(uint32_t form) const;
    uint32_t alias_ref(const Scene& s, int32_t alias);
    bool quest_running(const Scene& s);
    void set_playing(Scene& s, bool playing, const char* why = "ended");
    void begin(Scene& s);
    void update_start(Scene& s, float dt);
    bool update_random(Scene& s, float dt);
    void update_scene(Scene& s, float dt);
    bool update_action(Scene& s, Action& a, float dt);
    void action_start(Scene& s, Action& a, float dt);
    void action_update(Scene& s, Action& a, float dt);
    void action_end(Scene& s, Action& a);
    void action_cleanup(Scene& s, Action& a);
    void end_all_actions(Scene& s);
    // Runs the fragment, then `done`.
    void fragment(Scene& s, oss::FragmentKind kind, uint32_t index, int32_t entry, std::function<void()> done);

    QuestSystem& m_quests;
    SceneActors m_none;
    SceneActors* m_actors = &m_none;
    std::vector<Scene> m_scenes;
    std::unordered_map<uint32_t, size_t> m_index;
    bool m_random_busy = false; // 9: both across scenes
    float m_random_countdown = 0;
    std::vector<uint32_t> m_in_scene; // as of the last update
};

} // namespace spud::game
