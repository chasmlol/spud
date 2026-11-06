#pragma once

// The audio thread's state, shared by the system and its rule modules (music, ambience, footsteps). Private
// to src/audio; everything here runs on the audio thread unless marked otherwise.

#include "audio/clip.hpp"
#include "audio/conditions.hpp"
#include "audio/device.hpp"
#include "audio/sound.hpp"
#include "audio/system.hpp"
#include "core/game_random.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace spud::audio {

class MusicManager;
class Ambience;

// A play request from a rule module: what the game gets plus how it is started.
struct PlayRequest {
    uint32_t descriptor = osd::k_none;      // a SNDR, or
    std::filesystem::path file;             // a file with no descriptor (music tracks)
    uint32_t category = osd::k_none;        // for files
    uint32_t output_override = osd::k_none; // a node's output model replaces the descriptor's (player sounds)
    uint8_t priority = 0;
    uint32_t flags = 0; // extra the game flags (0x20 music voice)
    std::optional<glm::vec3> position;
    std::optional<glm::vec3> segment_end; // line emitters
    bool follow_listener = false;
    float volume = 1.0f;
    float fade_in = 0.0f; // FadeInPlay: seconds from 4000 mB
};

// The game's fade message (0x12 in the game's switch): the fade attenuation moves linearly in mB
// against the audio thread's tick count, at least 100 ms long, then an optional stop.
struct Fade {
    bool active = false;
    uint16_t from = 0, to = 0;
    float duration = 0.0f, elapsed = 0.0f;
    bool stop_after = false;
    bool pause_after = false; // category pause: follow-up message 5
};

struct LiveSound {
    std::unique_ptr<GameSound> sound;
    Fade fade;
    bool released = false; // FadeOutAndRelease: the handle is invalid at once, the sound fades on
    bool loading = true;
    bool play_when_loaded = true;
    float start_frequency = 1.0f;
};

struct Command {
    enum class Type : uint8_t {
        play,
        stop,
        position,
        listener,
        environment,
        music_add,
        music_remove,
        music_stop_all,
        footstep,
        impact,
        master_volume,
        category_volume,
        menu_mode,
        loading_menu,
        dialogue,
        collision,
        play_file,
        speak,
        animation_sound,
        hrtf,
    };
    Type type{};
    uint32_t id = 0;
    uint32_t a = 0, b = 0, c = 0;
    float f = 0;
    bool has_position = false;
    glm::vec3 position{0};
    glm::vec3 position2{0}; // animation sounds: SoundPlayAt's node
    Listener listener;
    Environment env;
    bool flag = false, flag2 = false;
    char tag[160] = {}; // footstep tags; play_file's cache path
};

// Single-producer (the engine's main loop), single-consumer (the audio thread) ring. Never blocks.
class CommandRing {
  public:
    bool push(const Command& c) {
        const size_t w = m_write.load(std::memory_order_relaxed);
        if (w - m_read.load(std::memory_order_acquire) >= m_items.size()) {
            return false;
        }
        m_items[w % m_items.size()] = c;
        m_write.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(Command& out) {
        const size_t r = m_read.load(std::memory_order_relaxed);
        if (r == m_write.load(std::memory_order_acquire)) {
            return false;
        }
        out = m_items[r % m_items.size()];
        m_read.store(r + 1, std::memory_order_release);
        return true;
    }

  private:
    std::array<Command, 4096> m_items;
    std::atomic<size_t> m_write{0}, m_read{0};
};

// Reads and decodes audio files off the audio thread.
class Loader {
  public:
    struct Result {
        uint32_t sound = 0;
        std::string key; // the file
        std::shared_ptr<const Clip> clip;
        std::unique_ptr<ClipStream> stream;
        std::string error;
    };
    Loader();
    ~Loader();
    void request(uint32_t sound, std::filesystem::path file);
    std::vector<Result> take();

  private:
    void run(std::stop_token stop);
    std::mutex m_mutex;
    std::condition_variable_any m_cv;
    std::deque<std::pair<uint32_t, std::filesystem::path>> m_queue;
    std::vector<Result> m_done;
    std::unordered_map<std::string, std::weak_ptr<const Clip>> m_cache;
    std::deque<std::shared_ptr<const Clip>> m_recent; // keeps recently used clips alive
    size_t m_recent_bytes = 0;
    std::jthread m_thread;
};

struct AudioSystem::Impl {
    explicit Impl(AudioSystem& owner);
    ~Impl();

    // --- audio thread ---
    void run(std::stop_token stop);
    void tick(float dt);
    void handle(const Command& c);

    // Starts a sound of a descriptor; returns its id. Conditions are the caller's business.
    uint32_t start_sound(const PlayRequest& request, uint32_t id = 0);

    // The game's sound cache:
    // which files it holds decides what plays when a sound's own file isn't cached yet. It is a model of the
    // game's bookkeeping only; the decoded data lives in the loader's clip cache either way.
    struct CacheEntry {
        std::string key;     // the file
        uint32_t descriptor; // the SNDR whose sound added it
        uint32_t bytes;      // the game's data size (the WAV data chunk)
    };
    // Speech highlight: non-dialogue speech plays in one of two
    // code-side sub-categories of AudioCategoryNonVOC, by the speaker's distance to the listener.
    uint32_t speech_highlight = osd::k_none; // index in categories (after the records' own)
    uint32_t speech_non_highlight = osd::k_none;
    std::vector<uint32_t> speeches; // live speech sounds
    bool speech_any_highlighted = false;
    void update_speech();

    // Animation sound events: per reference, the handle of
    // each key (the event's payload).
    enum class AnimationSoundOp : uint8_t { play, stop, release, end, move };
    std::unordered_map<uint32_t, std::unordered_map<std::string, uint32_t>> animation_sounds;
    void animation_sound(const Command& c);
    // The game: |dx|, |dy| and |dz| from the listener each under the max
    // distance (the output model's when it attenuates, else unbounded).
    bool descriptor_in_range(uint32_t descriptor, const glm::vec3& position) const;

    std::list<CacheEntry> sound_cache; // most recently used first
    std::unordered_map<std::string, std::list<CacheEntry>::iterator> sound_cache_index;
    uint64_t sound_cache_bytes = 0;
    std::unordered_map<uint32_t, uint32_t> cache_loads; // QueueCacheLoad jobs: loader id -> descriptor
    uint32_t next_cache_load = 0x80000000u;
    void cache_add(const std::string& key, uint32_t descriptor, uint32_t bytes, uint8_t priority);
    std::optional<std::string> cache_substitute(uint32_t descriptor, const std::string& picked);
    // What the condition functions see now: the player's world as the engine last described it.
    ConditionContext condition_context() const;
    void stop_sound(uint32_t id);
    void fade_out_and_release(uint32_t id, float seconds); // to 4000 mB, then stop
    void fade_to_attenuation(uint32_t id, uint16_t mb, float seconds);
    bool sound_valid(uint32_t id) const; // playing or loading, not released
    uint32_t sound_position_ms(uint32_t id) const;
    void sound_seek_ms(uint32_t id, uint32_t ms);
    void sound_pause(uint32_t id, bool pause);
    // SetVolume (message 13): only when it moved by more than `epsilon`.
    void sound_set_volume(uint32_t id, float volume, float epsilon);
    void sound_set_position(uint32_t id, const glm::vec3& position);
    LiveSound* live(uint32_t id);
    SoundSetup setup_for(const PlayRequest& request);
    uint16_t category_attenuation(uint32_t index) const;
    bool category_paused(uint32_t index) const;
    bool category_within(uint32_t category, uint32_t ancestor) const; // IsSameOrChildOf
    uint32_t category_by_form(uint32_t form_id) const;
    uint32_t default_object(const char (&signature)[5]) const; // DOBJ FormID, 0 if none
    // The game/Unpause/Mute/Unmute/SetAttenuation: change the category,
    // then fade every sound in its subtree to its category attenuation over max(seconds, 0.1).
    void category_pause(uint32_t category, bool pause, float seconds);
    void category_mute(uint32_t category, bool mute, float seconds);
    void category_set_attenuation(uint32_t category, uint16_t mb, float seconds);
    void broadcast_category_fade(uint32_t category, float seconds, bool pause_after, bool unpause);
    bool menu_mode = false;
    float setting(std::string_view name, float fallback) const;
    std::filesystem::path file_path(uint32_t string_offset) const;
    // The game's shared generator (core/game_random.hpp). The game draws file choice, pitch and dB variance
    // (CalculateFrequencyRatio, ApplyDescriptorParams), music picks, region one-shots and their placement
    // from it.
    float random01() { return static_cast<float>(GameRandom::audio().next_unit()); }
    uint32_t random_below(uint32_t n) { return GameRandom::audio().next_below(n); }

    AudioSystem& owner;
    const osd::SoundDb& db;
    std::unique_ptr<Device> device;
    Loader loader;
    CommandRing commands;
    std::unordered_map<uint32_t, LiveSound> sounds;
    std::vector<uint32_t> last_file; // per descriptor: the last file picked
    std::vector<CategoryState> categories;
    std::unordered_map<std::string, float> settings;
    std::atomic<float> master_volume{1.0f};
    std::vector<std::atomic<float>> menu_volumes; // per category, menu value
    Listener listener;
    Environment env;
    std::atomic<uint32_t> next_id{1};

    std::unique_ptr<MusicManager> music;
    std::unique_ptr<Ambience> ambience;

    // Snapshot for state_json (published by the audio thread, read with try_lock by the main thread).
    mutable std::mutex snapshot_mutex;
    std::string snapshot;
    mutable std::string snapshot_copy;
    std::atomic<uint64_t> ticks{0};
    double time = 0.0; // seconds of audio-thread time (the game's GetTickCount role)

    std::atomic<Device*> device_ready{nullptr}; // set once the audio thread has opened the device
    std::atomic<bool> device_failed{false};
    std::jthread thread;
};

} // namespace spud::audio
