#pragma once

// Dialogue. Topics and INFOs come from the
// forms database, what a line does to the world goes through DialogueOutput, and scenes reach it through SceneActors.

#include "core/game_random.hpp"
#include "game/quests.hpp"
#include "game/scenes.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace spud::game {

enum InfoFlag : uint32_t {
    info_goodbye = 0x1, info_random = 0x2, info_say_once = 0x4, info_refusal = 0x10, info_random_end = 0x20,
    info_invisible_continue = 0x40, info_walk_away_hidden = 0x100, info_force_subtitle = 0x200, info_no_lip = 0x800,
    info_used_this_cycle = 0x8000
};
enum BranchFlag : uint32_t { branch_top_level = 0x1, branch_blocking = 0x2, branch_exclusive = 0x4 };
enum TopicCategory : uint8_t { category_topic = 0, category_favor = 1, category_scene = 2, category_combat = 3, category_misc = 7 };
inline constexpr uint16_t k_subtype_rumors = 2, k_subtype_goodbye = 78, k_subtype_hello = 79;

struct Response {
    uint32_t emotion = 0, emotion_value = 0;
    uint32_t number = 0;
    uint32_t sound = 0;
    bool use_emotion = false;
    std::string text;
    uint32_t speaker_idle = 0, listener_idle = 0;
};

struct Topic;

struct Info {
    uint32_t form = 0;
    Topic* topic = nullptr;
    uint32_t flags = 0;
    uint32_t reset = 0;       // ENAM: a fraction of a day, value x 24 / 65535 hours
    uint32_t previous = 0;    // PNAM
    uint32_t shared = 0;      // DNAM
    uint32_t speaker = 0;     // ANAM
    uint32_t walk_away = 0;   // TWAT
    std::string prompt;       // RNAM
    std::vector<uint32_t> links; // TCLT
    std::vector<ConditionItem> conditions;
    std::vector<Response> responses;
    bool said = false;
};

struct Topic {
    uint32_t form = 0;
    std::string editor_id, text;
    uint8_t flags = 0, category = 0, priority = 0;
    uint16_t subtype = 0;
    uint32_t branch = 0, quest = 0;
    std::vector<uint32_t> info_forms; // record order; `infos` once loaded
    std::vector<Info*> infos;
    bool loaded = false;
};

struct Branch {
    uint32_t form = 0, flags = 0, quest = 0, start = 0, type = 0;
};

// What a line does outside the rules: audio, the menu, subtitles.
class DialogueOutput {
public:
    virtual ~DialogueOutput() = default;
    // A voice file's length in seconds, 0 when there's none (3.5, 3.6).
    virtual float voice_length(std::string_view) { return 0; }
    virtual void play_voice(uint32_t /*speaker*/, std::string_view /*path*/, bool /*in_menu*/, uint32_t /*emotion*/,
                            uint32_t /*strength*/) {}
    virtual void stop_voice(uint32_t) {}
    virtual void follow_voice(uint32_t /*speaker*/) {} // per frame while a line runs: the sound stays on the speaker
    virtual void add_subtitle(uint32_t /*speaker*/, std::string_view /*text*/, bool /*forced*/) {}
    virtual void remove_subtitle(uint32_t) {}
    // BribeCost, CrimeGold, CrimeGoldViolent or CrimeGoldNonviolent for the speaker; below 0 for none: the tag stays.
    virtual int prompt_tag(std::string_view /*tag*/) { return -1; }
    // The voice type's editor ID of a speaker; "" for none.
    virtual std::string voice_type(uint32_t /*speaker*/) { return {}; }
    struct Row {
        std::string text;
        bool is_new = false;
        int index = 0;
    };
    virtual void open_menu(std::string_view /*name*/) {}
    virtual void close_menu() {}
    virtual void show_text(std::string_view) {}
    // `refreshed`: the list after a line; false with the opening one, which shows beside the greeting.
    virtual void show_topics(const std::vector<Row>&, bool refreshed) { (void)refreshed; }
    virtual void voice_ready() {}
};

class Dialogue final : public SceneActors {
public:
    Dialogue(QuestSystem& quests, DialogueOutput& output);

    // 3.1, 3.4, 1.5.
    Info* pick_info(Topic& topic, uint32_t speaker, uint32_t target, bool walking = false, const Topic* previous = nullptr);
    Info* pick_by_subtype(uint8_t category, uint16_t subtype, uint32_t speaker, uint32_t target);
    Info* pick_greeting(uint32_t speaker);

    // 3.5, 3.6.
    std::string voice_path(const Info& info, const Response& response, uint32_t speaker);
    float silent_seconds(const Response& response) const;

    // The conversation with the player (1.3 to 1.8, 2, 4.3).
    bool start_conversation(uint32_t speaker, Info* forced = nullptr);
    void choose_topic(int index);
    void skip();
    void close();
    void update(float dt, double game_days);
    bool in_conversation() const { return m_speaker != 0; }
    uint32_t speaker_ref() const { return m_speaker; }
    std::span<const DialogueOutput::Row> rows() const { return m_shown; }

    // A line outside the menu (4.1); false if the speaker is busy or the topic has nothing to say.
    bool say(uint32_t speaker, uint32_t topic, uint32_t target, bool no_padding = false);
    bool is_saying_line(uint32_t speaker) const;

    // SceneActors: scenes talk through the same lines.
    bool is_actor(uint32_t ref) override;
    bool start_saying(uint32_t speaker, uint32_t topic, uint32_t target) override { return say(speaker, topic, target); }
    bool is_saying(uint32_t speaker) override { return is_saying_line(speaker); }
    uint32_t speaker() override { return m_speaker; }
    bool conversation_active() override { return m_speaker != 0; }

    Topic* topic(uint32_t form);
    Info* info(uint32_t form);
    // For checks: why each top-level topic does or doesn't reach the list, one line each.
    std::vector<std::string> explain(uint32_t speaker);

private:
    struct Entry {
        Topic* topic = nullptr;
        Info* info = nullptr;
        std::string text;
        bool hidden = false;
    };
    // One line being said: the responses of an INFO with a cursor.
    struct Line {
        uint32_t speaker = 0, target = 0;
        Info* info = nullptr;
        size_t cursor = 0;
        float timer = 0;
        float gap = 0;
        bool in_menu = false;
        bool begun = false;
        bool no_padding = false; // the actor flag DoNotPadVoice
        bool last = false;       // the conversation ended under it: this response is the last
    };

    void load_records();
    void load_infos(Topic& t);
    Info* load_info(uint32_t form, Topic& t);
    bool info_passes(const Quest& q, const Info& i, uint32_t speaker, uint32_t target);
    bool quest_open(const Quest* q) const;
    bool remembered(const Info& i) const;

    void collect_branches();
    void build_list();
    void add_topic(Topic& t, const Topic* previous, bool links);
    void sort_rows();
    std::string replace_tags(std::string text, const Topic& t);
    void use_entry(const Entry& e, bool speak);
    void send_list(bool opening = false);
    void speak_current();
    void line_finished();
    void end_conversation();
    void run_fragment(Info& info, oss::FragmentKind kind, uint32_t speaker, std::function<void()> done);
    const Response* response(const Info& i, size_t index);
    void bind_natives();

    QuestSystem& m_quests;
    DialogueOutput& m_output;
    std::unordered_map<uint32_t, std::unique_ptr<Topic>> m_topics;
    std::unordered_map<uint32_t, std::unique_ptr<Info>> m_infos;
    std::unordered_map<uint32_t, Branch> m_branches;
    std::unordered_map<uint32_t, std::vector<Topic*>> m_quest_topics;
    std::unordered_map<uint32_t, std::vector<Branch*>> m_quest_branches;
    std::unordered_map<uint32_t, double> m_reset_stamps; // INFO -> game days when it ran (2.6)
    double m_days = 0;
    std::mt19937 m_random{session_seed()};

    uint32_t m_speaker = 0;
    std::vector<Branch*> m_top_level, m_blocking;
    std::vector<Entry> m_list;
    std::vector<DialogueOutput::Row> m_shown;
    std::optional<Entry> m_current;
    Info* m_link_source = nullptr;
    bool m_blocking_found = false;
    uint32_t m_forced_branch = 0;
    bool m_goodbye = false;
    std::vector<Topic*> m_history;
    std::vector<Line> m_lines;
    bool m_end_fragment = false; // a menu line's end fragment is running
    bool m_list_due = false;     // it finished: the conversation goes on at the next update
    bool m_rumors = false;
    std::string m_rumors_text;
};

} // namespace spud::game
