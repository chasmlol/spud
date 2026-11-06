#pragma once

// spud's audio system: Skyrim's sounds, music and ambience from the import cache (audio/sound.osd plus the
// converted sound and music files), mixed by XAudio2 the way the game mixes them. docs/AUDIO.md is the
// overview the facts it implements.
//
// Threads: the main thread only enqueues commands and reads the last published snapshot; it never waits on
// audio. The audio thread runs the game's audio update (every uAudioThreadSleepTimeGameMode ms, like Skyrim's
// The game), a loader thread reads and decodes files, and XAudio2 mixes on its own thread.

#include "audio/sound.hpp"
#include "formats/osd.hpp"

#include <glm/vec3.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spud::audio {

struct AudioOptions {
    std::filesystem::path cache;    // contains audio/sound.osd, sound/, music/
    bool silent = false;            // nothing to the speakers (captures still record)
};

// What the world looks like to the audio rules (ambience, music, reverb). Set by the engine each frame.
struct Environment {
    uint32_t world = 0;        // WRLD FormID (exteriors)
    uint32_t interior = 0;     // interior CELL FormID, 0 outside
    float game_hour = 12.0f;   // 0..24
    uint32_t weather = 0;      // current WTHR FormID (0: none)
    uint32_t last_weather = 0; // outgoing WTHR during a transition (0: none)
    float weather_pct = 1.0f;  // the transition fraction (Sky)
    bool underwater = false;   // the camera is under water
    // Water ambience, sampled by the engine like the game job.
    uint32_t water = 0;         // WATR FormID of the water at the listener, 0 = none
    uint32_t water_samples = 0; // wet grid points kept (0 = fade out)
    glm::vec3 water_emitter{0}; // their smoothed centroid, metres
};

// Identifies a sound started through the AudioSystem. 0 is "none".
using SoundId = uint32_t;

class AudioSystem {
  public:
    explicit AudioSystem(AudioOptions options);
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // False when the cache has no sound database or no audio device could be opened; every call is then a
    // no-op and the engine runs silent.
    bool available() const { return m_available; }
    const osd::SoundDb& db() const { return m_db; }

    // Lookups for scripts and tools: EditorID (case-insensitive) or FormID ("0x12345" / "12345").
    std::optional<uint32_t> find_descriptor(std::string_view name_or_form) const;
    std::optional<uint32_t> find_music_type(std::string_view name_or_form) const;
    std::optional<uint32_t> find_category(std::string_view name_or_form) const;
    std::optional<uint32_t> find_output_model(std::string_view name_or_form) const;
    std::optional<uint32_t> find_cell(std::string_view name_or_form) const;
    std::optional<uint32_t> find_weather(std::string_view name_or_form) const;
    std::optional<uint32_t> find_footstep_set(std::string_view name_or_form) const;
    std::optional<uint32_t> find_impact_set(std::string_view name_or_form) const;
    std::optional<uint32_t> find_material(std::string_view name_or_form) const;

    // Water at a point of an exterior worldspace, for the engine's water ambience sampling: the
    // cell's water (else the worldspace's), whether the cell has water and its height, and the sampling
    // radius (the water sound's max distance). Pure lookups in the database: safe on any thread.
    struct WaterQuery {
        uint32_t water = 0; // WATR FormID, 0 = none
        bool has_water = false;
        std::optional<float> height; // metres, when the cell stores one
        float radius = 0.0f;         // metres
    };
    WaterQuery water_at(uint32_t world, float x, float y) const;

    // --- sounds (any thread that owns the engine's main loop; never blocks) ---
    // Plays a descriptor (index into db().descriptors) at a world position (metres), or 2D when nullopt.
    // `output_model` in place of the descriptor's, k_none to keep it.
    SoundId play(uint32_t descriptor, std::optional<glm::vec3> position = std::nullopt, uint32_t output_model = osd::k_none);
    // A file without a descriptor, e.g. a dialogue line's.ogg: a
    // cache path, its category, and the output model the caller sets on it (
    // dialogue uses DOBJ 147/148 E), or k_none for none. Long files stream.
    SoundId play_file(std::string_view cache_path, uint32_t category, uint32_t output_model,
                      std::optional<glm::vec3> position = std::nullopt);
    // Speech that isn't the player's conversation (greetings, chatter, combat
    // taunts): a voice file at the speaker, with SOMDialogue3DDefault unless given another output model, put
    // in the speech highlight or non-highlight sub-category of AudioCategoryNonVOC by the speaker's distance
    // to the listener. Move it with set_position as the speaker moves.
    // In dialogue, or the player's, it goes in DOBJ DDSC (AudioCategoryVOC) instead (
    // 4.2 step 8). Without a position it is the player's: SOMDialogue2D, DOBJ DOP3.
    SoundId speak(std::string_view cache_path, std::optional<glm::vec3> position,
                  uint32_t output_model = osd::k_none, bool in_dialogue = false);
    // The node output model of the player's body, which replaces a sound's own at the player: DOBJ POPM or P3OM.
    uint32_t player_output_model(bool first_person) const;
    void stop(SoundId id);
    void set_volume(SoundId id, float volume);
    void set_position(SoundId id, const glm::vec3& position);

    // --- the world ---
    void set_listener(const Listener& listener);
    void set_environment(const Environment& env);

    // --- music ---
    void music_add(uint32_t music_type); // push a music type, as the game's MusicType.Add
    void music_remove(uint32_t music_type);
    void music_stop_all();

    // --- gameplay triggers (docs/AUDIO.md: hooks for animation, physics and combat) ---
    // A footstep animation event (the game's footstep handler): the actor's footstep set (index into
    // db().footstep_sets; osd::k_none = the default set), gait, the event tag ("FootLeft", "JumpDown", ...),
    // the ground material (MATT FormID) and the actor's position. The player plays IPCT Sound 2 with the
    // player output model of the camera mode; NPCs play Sound 1 through an 8-slot pool.
    void footstep(uint32_t footstep_set, osd::FootstepGait gait, std::string_view tag, uint32_t material,
                  const glm::vec3& position, bool player = false, bool first_person = false);
    // A projectile or explosion impact (PlayImpactEffect): both IPCT sounds at the point.
    void impact(uint32_t impact_set, uint32_t material, const glm::vec3& position);
    // A weapon hit: Sound 1, or Sound 2 for the player's normal hits.
    void combat_impact(uint32_t impact_set, uint32_t material, const glm::vec3& position,
                       bool player_attacker, bool normal_hit);
    // A physics contact between two materials (MATT FormIDs) at `speed` m/s.
    void collision(uint32_t material_a, uint32_t material_b, const glm::vec3& position, float speed);
    // The MATT whose material ID (the game of its lower-cased MNAM) a Havok material carries; 0 if none.
    uint32_t material_from_havok_id(uint32_t havok_material) const;
    // A sound annotation from a reference's animation (the game's animation-graph sink):
    // "SoundPlay.<EDID>" (a SNDR, or a SOUN's), "SoundPlayAt.<EDID>.<node>", "SoundStop.<key>",
    // "SoundRelease.<key>" or "SoundAnimationEnd". `position` is the reference's (the range test's); `node`,
    // where SoundPlayAt's node is. The sounds keep a handle per reference and key, as the game's handler does
    // Returns false for annotations that aren't sound events.
    bool animation_event(uint32_t reference, std::string_view annotation, const glm::vec3& position,
                         std::optional<glm::vec3> node = std::nullopt);
    // The game's animation sounds follow their reference's 3D: move them with it.
    void move_animation_sounds(uint32_t reference, const glm::vec3& position);

    // --- game state the audio rules react to ---
    void set_menu_mode(bool open);    // menus pause and fade categories
    void set_loading_menu(bool open); // the loading screen pauses its category
    void set_dialogue(bool open);     // the dialogue menu ducks non-dialogue sounds

    // --- settings (the Audio menu) ---
    // The game's Audio menu: the master volume and one slider per category flagged "Should Appear on Menu"
    // Values are linear gains 0..1; defaults 1.0 and the categories' UNAM.
    struct MenuSlider {
        uint32_t category; // index
        uint32_t form_id;  // the category's (the game's settings keep the levels by it)
        std::string
            key; // settings.ini [audio] key: effects, footsteps, voice, music, or the lower-cased EDID
        float default_value; // UNAM / 65535
    };
    std::vector<MenuSlider> menu_sliders() const;
    void set_master_volume(float value);
    void set_category_volume(uint32_t category, float value); // menu value 0..1
    float master_volume() const;
    float category_volume(uint32_t category) const;
    // Headphone spatial audio: positioned sounds through measured HRTFs instead of the game's 5.1 panning. An
    // upgrade over the game, off by default. Takes effect on sounds already playing.
    void set_hrtf(bool on);

    // --- automation ---
    // Records `seconds` of the 6-channel mix into a float WAV; done() turns true when written.
    bool capture(const std::filesystem::path& file, double seconds);
    bool capture_busy() const;
    std::string state_json() const;

    struct Impl;

  private:
    // EditorID (lower-case) and FormID -> index, per record table (keyed by the table's address), built at
    // load so lookups cost a hash, not a scan: scripts and tools look sounds up by name every time they play
    // one.
    struct NameIndex {
        std::unordered_map<std::string, uint32_t> names;
        std::unordered_map<uint32_t, uint32_t> forms;
    };
    template <typename T>
    std::optional<uint32_t> find_record(const std::vector<T>& items, std::string_view key) const;
    std::unordered_map<const void*, NameIndex> m_names;

    AudioOptions m_options;
    osd::SoundDb m_db;
    bool m_available = false;
    std::unique_ptr<Impl> m_impl;
    std::unordered_map<uint64_t, uint32_t> m_exterior_cells; // (world, x, y) -> cell index
};

} // namespace spud::audio
