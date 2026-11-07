// The dialogue system's outputs: the Dialogue Menu, voice lines through the audio system, the HUD's subtitles.

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "engine/app_internal.hpp"
#include "ui/menu.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <future>
#include <optional>
#include <unordered_map>

namespace spud::engine {

namespace {

// An .ogg's length from its last page's granule position and the rate in its first page, without decoding it.
float ogg_seconds(const std::filesystem::path& file) {
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        return 0;
    }
    const auto bytes = read_file(file);
    const auto* d = reinterpret_cast<const uint8_t*>(bytes.data());
    if (bytes.size() < 64) {
        return 0;
    }
    uint32_t rate = 0;
    std::memcpy(&rate, d + 40, 4); // the identification header after a 28-byte page header and 12 of its own
    for (size_t at = bytes.size() - 14; rate && at > 0; --at) {
        if (std::memcmp(d + at, "OggS", 4) == 0) {
            uint64_t granule = 0;
            std::memcpy(&granule, d + at + 6, 8);
            return static_cast<float>(granule) / static_cast<float>(rate);
        }
    }
    return 0;
}

class EngineDialogue final : public game::DialogueOutput {
public:
    EngineDialogue(App& app, const forms::Database& db, ui::Ui* ui, audio::AudioSystem* sound, std::filesystem::path cache)
        : m_app(app), m_db(db), m_ui(ui), m_sound(sound), m_cache(std::move(cache)) {}

    int prompt_tag(std::string_view tag) override { return m_app.dialogue_prompt_tag(tag); }

    // Off the main thread: a cold file takes tens of milliseconds to open, and a line is said a frame or two later for it.
    std::optional<float> voice_length(std::string_view path) override {
        const std::string key(path);
        if (const auto it = m_voice_lengths.find(key); it != m_voice_lengths.end()) {
            return it->second;
        }
        auto [it, fresh] = m_voice_reads.try_emplace(key);
        if (fresh) {
            it->second = std::async(std::launch::async, ogg_seconds, m_cache / (key + ".ogg"));
        }
        if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return std::nullopt;
        }
        float length = 0;
        try {
            length = it->second.get();
        } catch (const std::exception& e) {
            log::warn("voice {}: {}", key, e.what());
        }
        m_voice_reads.erase(it);
        m_voice_lengths.emplace(key, length);
        return length;
    }

    void play_voice(uint32_t speaker, std::string_view path, bool in_menu, uint32_t emotion, uint32_t strength) override {
        const bool player = speaker == game::k_player_ref;
        if (m_sound && (player || m_app.speaker_here(speaker))) {
            // the player's is 2D and VOC; an NPC's is at the speaker, VOC in dialogue
            // and non-dialogue speech otherwise.
            const std::optional<glm::vec3> at = player ? std::nullopt : m_app.speaker_position(speaker);
            m_voices[speaker] = m_sound->speak(std::string(path) + ".ogg", at, osd::k_none, in_menu || player || !at);
        }
        m_app.actor_speak(speaker, std::string(path), static_cast<int>(emotion), static_cast<int>(strength));
    }

    void follow_voice(uint32_t speaker) override {
        if (const auto it = m_voices.find(speaker); it != m_voices.end() && m_sound && speaker != game::k_player_ref) {
            if (const std::optional<glm::vec3> at = m_app.speaker_position(speaker)) {
                m_sound->set_position(it->second, *at);
            }
        }
    }

    void stop_voice(uint32_t speaker) override {
        m_app.actor_speak(speaker, {}, 0, 0);
        if (auto it = m_voices.find(speaker); it != m_voices.end() && m_sound) {
            m_sound->stop(it->second);
            m_voices.erase(it);
        }
    }

    std::string voice_type(uint32_t speaker) override {
        // The player speaks with their race's default voice by sex: default objects PVMA and PVFA are 0 in every master, and the
        // lookup (the game slot 73) passes over the record's own.
        if (speaker == game::k_player_ref) {
            if (const forms::FormEntry* race = m_db.find(m_app.game_state()->combat().fighter(speaker).race)) {
                const forms::Form r(m_db, *race);
                const forms::Value* voices = r.get("VTCK");
                const forms::Value* voice = voices ? voices->member(m_app.game_state()->character().female() ? "female" : "male") : nullptr;
                if (const forms::FormEntry* type = voice ? m_db.find(voice->form) : nullptr) {
                    return std::string(m_db.editor_id(*type));
                }
            }
        }
        const forms::FormEntry* e = m_db.find(speaker);
        if (e && e->type == forms::sig("ACHR")) {
            const forms::Form ref(m_db, *e);
            const forms::Value* base = ref.get("NAME");
            e = base ? m_db.find(base->form) : nullptr;
        }
        if (!e) {
            return {};
        }
        const forms::Form npc(m_db, *e);
        const forms::Value* voice = npc.get("VTCK");
        const forms::FormEntry* type = voice ? m_db.find(voice->form) : nullptr;
        return type ? std::string(m_db.editor_id(*type)) : std::string();
    }

    ui::Menu* menu() { return m_ui ? m_ui->find_menu("Dialogue Menu") : nullptr; }

    void open_menu(std::string_view name) override {
        if (m_ui) {
            m_ui->open("Dialogue Menu");
            if (ui::Menu* m = menu()) {
                m->dialogue_speaker(std::string(name));
            }
        }
    }
    void close_menu() override {
        if (m_ui) {
            m_ui->close("Dialogue Menu");
        }
    }
    void show_text(std::string_view text) override {
        if (ui::Menu* m = menu()) {
            m->dialogue_text(std::string(text));
        }
    }
    void show_topics(const std::vector<Row>& rows, bool refreshed) override {
        if (ui::Menu* m = menu()) {
            std::vector<ui::DialogueRow> out;
            for (const Row& r : rows) {
                out.push_back({r.text, r.is_new, r.index});
            }
            m->dialogue_topics(out, refreshed);
        }
    }
    void voice_ready() override {
        if (ui::Menu* m = menu()) {
            m->dialogue_voice_ready();
        }
    }

private:
    App& m_app;
    const forms::Database& m_db;
    ui::Ui* m_ui;
    audio::AudioSystem* m_sound;
    std::filesystem::path m_cache;
    std::unordered_map<std::string, float> m_voice_lengths;
    std::unordered_map<std::string, std::future<float>> m_voice_reads;
    std::unordered_map<uint32_t, audio::SoundId> m_voices;
};

} // namespace

void App::attach_dialogue() {
    m_dialogue_output = std::make_unique<EngineDialogue>(*this, m_papyrus->quests()->db(), m_ui.get(), m_audio.get(), m_opt.cache);
    m_papyrus->attach_dialogue(*m_dialogue_output);
}

// The quests and dialogue of a game, once: the first thing that needs them starts them.
void App::start_quests() {
    if (m_quests_started) {
        return;
    }
    m_quests_started = true;
    if (m_game) {
        m_game->navmeshes(); // the table of them takes most of a second: not in the first tick
    }
    papyrus_call(""); // makes the VM when nothing has yet
    attach_dialogue();
    m_papyrus->new_game(std::exchange(m_intro_quest, 0));
    // Start-game quests reach the running list in a few teller updates; the first conversation needs them there.
    for (int i = 0; i < 4; ++i) {
        m_papyrus->update(0.0f, false);
    }
}

void App::ui_dialogue_topic(int index) {
    m_papyrus->dialogue()->choose_topic(index);
}

void App::ui_dialogue_skip() {
    m_papyrus->dialogue()->skip();
}

void App::ui_dialogue_closed() {
    if (m_papyrus && m_papyrus->dialogue() && m_papyrus->dialogue()->in_conversation()) {
        m_papyrus->dialogue()->close();
    }
}

void App::dialogue_look(float dt) {
    if (!m_game || !m_ui || !m_ui->is_open("Dialogue Menu")) {
        return;
    }
    const game::Settings& s = m_game->settings();
    if (!m_ui->gamepad_active()) {
        // Only the cursor looks, near an edge, away from the screen's middle. The game's is a value a frame at its
        // 60 frames a second: here so many a second, to turn as fast at any frame rate.
        constexpr float k_game_fps = 60.0f;
        const glm::vec2 size(m_ui->target_width(), m_ui->target_height()), at(m_ui->mouse_x(), m_ui->mouse_y());
        const float edge = s.number("fPCDialogueLookStart", 25.0f);
        const bool panning = at.x > size.x - edge || at.x < edge || at.y > size.y - edge || at.y < edge;
        const float counts = s.number("fPCDialogueLookSpeed", 10.0f) * k_game_fps * dt;
        m_walk_look = panning ? glm::normalize(at - size * 0.5f) * counts : glm::vec2(0.0f);
    }
    // Away from the speaker the turn fades out past the soft stop, and ends at the hard one.
    ActorSet* actors = m_space ? m_space->actors() : nullptr;
    game::Dialogue* talk = m_papyrus ? m_papyrus->dialogue() : nullptr;
    if (!actors || !talk || !m_player) {
        return;
    }
    const auto& who = actors->who();
    const auto speaker = std::ranges::find(who, talk->speaker_ref(), &ActorSet::Who::ref);
    if (speaker == who.end()) {
        return;
    }
    const glm::vec3 to = speaker->position - m_player->feet(), ahead = m_camera.forward();
    const float turned = std::atan2(to.x, to.y) - std::atan2(ahead.x, ahead.y);
    const float error = std::remainder(turned, 2.0f * glm::pi<float>()); // to the speaker, clockwise
    if (error * m_walk_look.x >= 0.0f) {
        return;
    }
    const bool first = m_player->view() == View::first_person;
    const float soft = glm::radians(first ? s.number("fDialogueSoftStopAngle1P", 20.0f)
                                          : s.number("fDialogueSoftStopAngle3P", 25.0f));
    const float hard = glm::radians(first ? s.number("fDialogueHardStopAngle1P", 45.0f)
                                          : s.number("fDialogueHardStopAngle3P", 55.0f));
    const float off = std::abs(error);
    if (off > hard) {
        m_walk_look.x = 0.0f;
    } else if (off > soft) {
        const float dampen = s.number("fOutsideDialogueAngleRotationDampen", 0.33f);
        m_walk_look.x *= dampen * (1.0f - (off - soft) / (hard - soft));
    }
}

int App::dialogue_prompt_tag(std::string_view tag) {
    game::Dialogue* talk = m_papyrus ? m_papyrus->dialogue() : nullptr;
    const uint32_t speaker = talk ? talk->speaker_ref() : 0;
    if (!m_game || !speaker) {
        return -1;
    }
    if (tag == "BribeCost") {
        return m_game->crime().bribe_amount(m_game->base_of(speaker));
    }
    const game::Bounty& b = m_game->crime().bounty(m_game->crime_faction(speaker));
    return tag == "CrimeGoldViolent" ? b.violent : tag == "CrimeGoldNonviolent" ? b.non_violent : b.total();
}

} // namespace spud::engine
