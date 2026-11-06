#include "audio/system.hpp"

#include "audio/ambience.hpp"
#include "audio/capture.hpp"
#include "audio/internal.hpp"
#include "audio/music.hpp"
#include "core/error.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <sstream>

namespace spud::audio {

namespace {

using Clock = std::chrono::steady_clock;

// Files above this many bytes of PCM are streamed rather than decoded whole (uStreamingThreshold:Audio is
// 4194304 in the game; the choice changes memory use, not what is heard).
constexpr uint64_t k_stream_threshold = 4194304;

std::optional<uint32_t> parse_form(std::string_view s) {
    if (s.starts_with("0x") || s.starts_with("0X")) {
        s.remove_prefix(2);
    }
    uint32_t v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (ec != std::errc() || p != s.data() + s.size() || s.empty()) {
        return std::nullopt;
    }
    return v;
}

std::string json_escape(std::string_view s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

} // namespace

// --- Loader ------------------------------------------------------------------------------------------

Loader::Loader() : m_thread([this](std::stop_token st) { run(st); }) {}

Loader::~Loader() {
    m_thread.request_stop();
    m_cv.notify_all();
}

void Loader::request(uint32_t sound, std::filesystem::path file) {
    {
        std::lock_guard lock(m_mutex);
        m_queue.emplace_back(sound, std::move(file));
    }
    m_cv.notify_one();
}

std::vector<Loader::Result> Loader::take() {
    std::lock_guard lock(m_mutex);
    return std::exchange(m_done, {});
}

void Loader::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::pair<uint32_t, std::filesystem::path> job;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, stop, [&] { return !m_queue.empty(); });
            if (stop.stop_requested()) {
                return;
            }
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }
        Result r;
        r.sound = job.first;
        r.key = job.second.string();
        try {
            const auto key = job.second.string();
            std::shared_ptr<const Clip> cached;
            {
                std::lock_guard lock(m_mutex);
                if (auto it = m_cache.find(key); it != m_cache.end()) {
                    cached = it->second.lock();
                }
            }
            if (cached) {
                r.clip = cached;
            } else {
                const auto bytes = std::filesystem::file_size(job.second);
                const bool ogg = job.second.extension() == ".ogg";
                // An Ogg file decodes to roughly ten times its size.
                if ((ogg ? bytes * 10 : bytes) > k_stream_threshold) {
                    r.stream = open_stream(job.second);
                } else {
                    r.clip = load_clip(job.second);
                    std::lock_guard lock(m_mutex);
                    m_cache[key] = r.clip;
                    m_recent.push_back(r.clip);
                    m_recent_bytes += r.clip->pcm.size() * 2;
                    while (m_recent_bytes > (256u << 20) && !m_recent.empty()) {
                        m_recent_bytes -= m_recent.front()->pcm.size() * 2;
                        m_recent.pop_front();
                    }
                }
            }
        } catch (const std::exception& e) {
            r.error = e.what();
        }
        std::lock_guard lock(m_mutex);
        m_done.push_back(std::move(r));
    }
}

// --- Impl ---------------------------------------------------------------------------------------------

AudioSystem::Impl::Impl(AudioSystem& owner_)
    : owner(owner_), db(owner_.m_db), menu_volumes(owner_.m_db.categories.size()) {
    last_file.assign(db.descriptors.size(), osd::k_none);
    categories.resize(db.categories.size());
    for (size_t i = 0; i < db.categories.size(); ++i) {
        const auto& c = db.categories[i];
        categories[i].parent = c.parent;
        categories[i].static_volume = c.static_volume;
        menu_volumes[i] = (c.flags & osd::category_on_menu) ? c.default_menu_value : 1.0f;
        categories[i].volume = menu_volumes[i];
    }
    for (const auto& s : db.settings) {
        settings[path::to_lower(db.str(s.name))] = s.value;
    }
    master_volume = setting("fAudioMasterVolume:AudioMenu", 1.0f);
    // The two speech-highlight sub-categories (NonDialogSubCategory, vtable): volume and pitch
    // are DOBJ NDSC's (AudioCategoryNonVOC), attenuation their own plus its.
    if (const uint32_t non_voc = category_by_form(default_object("NDSC")); non_voc != osd::k_none) {
        CategoryState c;
        c.parent = non_voc;
        speech_highlight = static_cast<uint32_t>(categories.size());
        categories.push_back(c);
        speech_non_highlight = static_cast<uint32_t>(categories.size());
        categories.push_back(c);
    }
    music = std::make_unique<MusicManager>(*this);
    ambience = std::make_unique<Ambience>(*this);
}

AudioSystem::Impl::~Impl() {
    thread.request_stop();
    if (thread.joinable()) {
        thread.join();
    }
}

float AudioSystem::Impl::setting(std::string_view name, float fallback) const {
    auto it = settings.find(path::to_lower(name));
    return it == settings.end() ? fallback : it->second;
}

std::filesystem::path AudioSystem::Impl::file_path(uint32_t string_offset) const {
    return owner.m_options.cache / db.str(string_offset);
}

LiveSound* AudioSystem::Impl::live(uint32_t id) {
    auto it = sounds.find(id);
    return it == sounds.end() ? nullptr : &it->second;
}

bool AudioSystem::Impl::sound_valid(uint32_t id) const {
    auto it = sounds.find(id);
    return it != sounds.end() && !it->second.released && !it->second.sound->finished();
}

uint32_t AudioSystem::Impl::sound_position_ms(uint32_t id) const {
    auto it = sounds.find(id);
    return it == sounds.end() || it->second.loading ? 0 : it->second.sound->position_ms();
}

void AudioSystem::Impl::sound_seek_ms(uint32_t id, uint32_t ms) {
    if (auto* s = live(id); s && !s->loading) {
        s->sound->seek_ms(ms);
    }
}

SoundSetup AudioSystem::Impl::setup_for(const PlayRequest& request) {
    SoundSetup s;
    s.position = request.position;
    s.segment_end = request.segment_end;
    s.follow_listener = request.follow_listener;
    s.volume = request.volume;
    if (request.descriptor == osd::k_none) {
        // A file without a descriptor: no output model, so no
        // attenuation and XAudio2's default output matrix.
        s.file = request.file;
        s.category = request.category;
        s.priority = request.priority;
        s.flags = 2 | request.flags;
        if (request.output_override != osd::k_none) {
            // The game: the output kind follows the model.
            s.output = &db.output_models[request.output_override];
            if (s.output->flags & osd::output_attenuates_with_distance) {
                s.flags =
                    (s.flags & ~7u) | (s.output->type == osd::OutputType::defined_speaker_output ? 4 : 1);
            }
        }
        return s;
    }
    const auto& d = db.descriptors[request.descriptor];
    s.descriptor = request.descriptor;
    // File: a random one, re-rolled to the next when it repeats the last pick (FillPlaybackParams,
    //).
    if (d.file_count > 0) {
        uint32_t pick = random_below(d.file_count);
        if (d.file_count > 1 && pick == last_file[request.descriptor]) {
            pick = (pick + 1) % d.file_count;
        }
        last_file[request.descriptor] = pick;
        s.file = file_path(db.files[d.first_file + pick]);
    }
    const uint32_t model = request.output_override != osd::k_none ? request.output_override : d.output_model;
    s.output = model != osd::k_none ? &db.output_models[model] : nullptr;
    s.category = d.category;
    uint32_t attenuation = 2;
    if (s.output && (s.output->flags & osd::output_attenuates_with_distance)) {
        attenuation = s.output->type == osd::OutputType::defined_speaker_output ? 4 : 1;
    }
    s.flags = (d.flags & 0xFFFFFFF8u) | attenuation | request.flags;
    // The game.
    s.frequency_base =
        static_cast<uint8_t>(std::clamp(d.frequency_shift * 0.01f + 1.0f, 0.1f, 2.0f) * 100.0f);
    s.frequency_variance = d.frequency_variance;
    s.priority = d.priority;
    uint32_t atten = d.static_attenuation;
    if (d.db_variance) {
        atten += random_below(static_cast<uint32_t>(d.db_variance * 100.0f));
    }
    s.static_attenuation = static_cast<uint16_t>(std::min<uint32_t>(atten, 0xFFFF));
    return s;
}

void AudioSystem::Impl::update_speech() {
    // The game, every frame: drop entries that ended (their end is 500 ms,
    // iHighlightSpeechOverlap, before the sound's), re-assign each by distance with a fHighlightSpeechOverlap
    // fade, and when "anyone highlighted" changes set the non-highlight sub-category's attenuation to
    // fNonHighlightSpeechAtten (12 dB) while someone is, else to 100 mB.
    if (speech_highlight == osd::k_none) {
        return;
    }
    const float radius = setting("fMaxHighlightRadius:Audio", 250.0f) * 0.0142875f;
    const float fade = setting("fHighlightSpeechOverlap:Audio", 0.5f);
    const auto overlap = static_cast<uint32_t>(setting("iHighlightSpeechOverlap:Audio", 500.0f));
    bool any = false;
    for (auto it = speeches.begin(); it != speeches.end();) {
        auto* s = live(*it);
        const bool ended = !s || s->sound->finished() ||
                           (!s->loading && s->sound->duration_ms() > 0 &&
                            s->sound->position_ms() + overlap >= s->sound->duration_ms());
        if (ended) {
            it = speeches.erase(it);
            continue;
        }
        const auto pos = s->sound->setup().position.value_or(listener.position);
        const uint32_t want =
            glm::distance(listener.position, pos) < radius ? speech_highlight : speech_non_highlight;
        any = any || want == speech_highlight;
        if (s->sound->setup().category != want) {
            // a playing sound fades to the new category's attenuation.
            s->sound->set_category(want);
            s->fade = {true,
                       s->sound->fade_attenuation(),
                       category_attenuation(want),
                       std::max(fade, 0.1f),
                       0.0f,
                       false,
                       false};
        }
        ++it;
    }
    if (any != speech_any_highlighted) {
        speech_any_highlighted = any;
        const auto atten = static_cast<uint16_t>(setting("fNonHighlightSpeechAtten:Audio", 12.0f) * 100.0f);
        category_set_attenuation(speech_non_highlight, any ? atten : 100, fade);
    }
}

bool AudioSystem::Impl::descriptor_in_range(uint32_t descriptor, const glm::vec3& position) const {
    float max_distance = std::numeric_limits<float>::max();
    const auto& d = db.descriptors[descriptor];
    if (d.output_model != osd::k_none) {
        const auto& om = db.output_models[d.output_model];
        if (om.flags & osd::output_attenuates_with_distance) {
            max_distance = om.max_distance;
        }
    }
    const glm::vec3 delta = position - listener.position;
    return std::fabs(delta.x) < max_distance && std::fabs(delta.y) < max_distance &&
           std::fabs(delta.z) < max_distance;
}

void AudioSystem::Impl::animation_sound(const Command& c) {
    // The game: a table of handles per reference (its FormID),
    // keyed by the event's payload.
    const auto ref = animation_sounds.find(c.a);
    switch (static_cast<AnimationSoundOp>(c.b)) {
    case AnimationSoundOp::play: {
        // The range test is at the reference's position. A key that already has a sound stops and releases it
        // first; the new one is built with flags 0x10 and follows the reference's 3D (or SoundPlayAt's node).
        if (!descriptor_in_range(c.c, c.position)) {
            return;
        }
        auto& slot = animation_sounds[c.a][c.tag];
        if (slot) {
            stop_sound(slot);
        }
        PlayRequest r;
        r.descriptor = c.c;
        r.position = c.flag ? c.position2 : c.position;
        r.flags = 0x10;
        slot = start_sound(r);
        return;
    }
    case AnimationSoundOp::stop:
        if (ref != animation_sounds.end()) {
            if (auto it = ref->second.find(c.tag); it != ref->second.end()) {
                stop_sound(it->second); // Stop: the key keeps its entry
            }
        }
        return;
    case AnimationSoundOp::release:
        if (ref != animation_sounds.end()) {
            if (auto it = ref->second.find(c.tag); it != ref->second.end()) {
                stop_sound(it->second);
                ref->second.erase(it);
            }
        }
        return;
    case AnimationSoundOp::end:
        // SoundAnimationEnd: every sound of the reference is stopped and released, and its table goes.
        if (ref != animation_sounds.end()) {
            for (const auto& [key, id] : ref->second) {
                stop_sound(id);
            }
            animation_sounds.erase(ref);
        }
        return;
    case AnimationSoundOp::move:
        if (ref != animation_sounds.end()) {
            for (const auto& [key, id] : ref->second) {
                sound_set_position(id, c.position);
            }
        }
        return;
    }
}

void AudioSystem::Impl::cache_add(const std::string& key, uint32_t descriptor, uint32_t bytes,
                                  uint8_t priority) {
    // Admission: the cache is enabled, the data is at most uMaxSizeForCachedSound, the sound's priority is at
    // most 128, the file isn't cached yet. LRU; the total stays within uiInitialCacheSize, skipping entries
    // in use.
    if (setting("bEnableAudioCache:Audio", 1.0f) == 0.0f ||
        bytes > static_cast<uint32_t>(setting("uMaxSizeForCachedSound:Audio", 262144.0f)) || priority > 128 ||
        sound_cache_index.contains(key)) {
        return;
    }
    sound_cache.push_front({key, descriptor, bytes});
    sound_cache_index[key] = sound_cache.begin();
    sound_cache_bytes += bytes;
    const auto limit = static_cast<uint64_t>(setting("uiInitialCacheSize:Audio", 1048576.0f));
    for (auto it = sound_cache.end(); sound_cache_bytes > limit && it != sound_cache.begin();) {
        --it;
        const bool in_use = std::any_of(sounds.begin(), sounds.end(), [&](const auto& s) {
            return s.second.sound->setup().file.string() == it->key && !s.second.sound->finished();
        });
        if (in_use || it == sound_cache.begin()) {
            continue;
        }
        sound_cache_bytes -= it->bytes;
        sound_cache_index.erase(it->key);
        it = sound_cache.erase(it);
    }
}

std::optional<std::string> AudioSystem::Impl::cache_substitute(uint32_t descriptor,
                                                               const std::string& picked) {
    if (setting("bEnableAudioCache:Audio", 1.0f) == 0.0f) {
        return std::nullopt;
    }
    if (auto hit = sound_cache_index.find(picked); hit != sound_cache_index.end()) {
        sound_cache.splice(sound_cache.begin(), sound_cache, hit->second); // the exact file: a hit
        return std::nullopt;
    }
    auto pick_from = [&](uint32_t d) -> std::optional<std::string> {
        std::vector<const CacheEntry*> found;
        for (const auto& e : sound_cache) {
            if (e.descriptor == d) {
                found.push_back(&e);
            }
        }
        if (found.empty()) {
            return std::nullopt;
        }
        return found[random_below(static_cast<uint32_t>(found.size()))]->key;
    };
    if (auto same = pick_from(descriptor)) {
        return same;
    }
    const uint32_t alternate = db.descriptors[descriptor].alternate_for;
    return alternate != osd::k_none ? pick_from(alternate) : std::nullopt;
}

ConditionContext AudioSystem::Impl::condition_context() const {
    ConditionContext c;
    c.game_hour = env.game_hour;
    c.weather = env.weather;
    c.interior = env.interior != 0;
    if (env.weather) {
        for (const auto& w : db.weathers) {
            if (w.form_id == env.weather) {
                // The game DATA byte 11: the classification (0x4 rainy, 0x8 snow).
                c.raining = (w.data[11] & 0x04) != 0;
                c.snowing = (w.data[11] & 0x08) != 0;
                break;
            }
        }
    }
    return c;
}

uint32_t AudioSystem::Impl::start_sound(const PlayRequest& request, uint32_t id) {
    if (request.descriptor != osd::k_none && request.descriptor >= db.descriptors.size()) {
        return 0;
    }
    if (request.descriptor != osd::k_none) {
        // The game evaluates the SNDR's conditions once, at build,
        // with the player as subject and target; when they fail no sound is built
        const auto& d = db.descriptors[request.descriptor];
        if (d.condition_count &&
            !evaluate(std::span(db.conditions).subspan(d.first_condition, d.condition_count),
                      condition_context())) {
            return 0;
        }
    }
    if (id == 0) {
        id = next_id++;
    }
    auto setup = setup_for(request);
    if (setup.file.empty()) {
        return 0;
    }
    if (request.descriptor != osd::k_none && path::to_lower(setup.file.extension().string()) == ".wav") {
        // The game: until its own file is cached, a sound plays a cached
        // file of the same SNDR, or of its "Alternate Sound For", while the real pick loads into the cache.
        // WAV files only: the game streams xWMA and never caches or substitutes streamed files.
        if (auto sub = cache_substitute(request.descriptor, setup.file.string())) {
            if (setup.priority <= 128) {
                const uint32_t job = next_cache_load++;
                cache_loads[job] = request.descriptor;
                loader.request(job, setup.file);
            }
            setup.file = *sub;
        }
    }
    LiveSound ls;
    // The game: base + uniform [-variance, +variance), times the
    // category's frequency multiplier.
    float ratio = setup.frequency_base * 0.01f;
    if (setup.frequency_variance) {
        const float v = setup.frequency_variance * 0.01f;
        ratio += random01() * 2.0f * v - v;
    }
    ratio *= effective_frequency(categories, setup.category);
    ls.start_frequency = ratio;
    const auto file = setup.file;
    const uint32_t category = setup.category;
    ls.sound = std::make_unique<GameSound>(id, std::move(setup));
    // Play adds the category's attenuation to the fade attenuation (message 4; GetAttenuation).
    uint16_t start_mb = 0;
    if (category != osd::k_none) {
        start_mb = category_attenuation(category);
    }
    if (request.fade_in > 0.0f) {
        // FadeInPlay: 4000 mB, Play (which adds the category's attenuation), then a fade to 0 mB.
        const auto from = static_cast<uint16_t>(std::min<uint32_t>(4000u + start_mb, 0xFFFFu));
        ls.sound->set_fade_attenuation(from);
        ls.fade = {true, from, 0, std::max(request.fade_in, 0.1f), 0.0f, false, false};
    } else {
        ls.sound->set_fade_attenuation(start_mb);
    }
    // A sound of a paused category pauses right after it starts.
    if (category != osd::k_none && category_paused(category)) {
        ls.play_when_loaded = false;
    }
    sounds.emplace(id, std::move(ls));
    loader.request(id, file);
    return id;
}

// The game: 4000 mB at a paused or muted category itself, otherwise its
// own attenuation plus its parent's (which may carry a parent's 4000), capped at 10000.
uint16_t AudioSystem::Impl::category_attenuation(uint32_t index) const {
    if (index == osd::k_none || index >= categories.size()) {
        return 0;
    }
    const auto& c = categories[index];
    if (c.paused || c.muted) {
        return 4000;
    }
    return static_cast<uint16_t>(std::min<uint32_t>(c.attenuation + category_attenuation(c.parent), 10000));
}

bool AudioSystem::Impl::category_paused(uint32_t index) const {
    for (int depth = 0; index != osd::k_none && index < categories.size() && depth < 32; ++depth) {
        if (categories[index].paused) {
            return true;
        }
        index = categories[index].parent;
    }
    return false;
}

bool AudioSystem::Impl::category_within(uint32_t category, uint32_t ancestor) const {
    for (int depth = 0; category != osd::k_none && category < categories.size() && depth < 32; ++depth) {
        if (category == ancestor) {
            return true;
        }
        category = categories[category].parent;
    }
    return false;
}

uint32_t AudioSystem::Impl::category_by_form(uint32_t form_id) const {
    for (uint32_t i = 0; i < db.categories.size(); ++i) {
        if (db.categories[i].form_id == form_id) {
            return i;
        }
    }
    return osd::k_none;
}

uint32_t AudioSystem::Impl::default_object(const char (&signature)[5]) const {
    for (const auto& d : db.default_objects) {
        if (std::memcmp(d.signature, signature, 4) == 0) {
            return d.form_id;
        }
    }
    return 0;
}

void AudioSystem::Impl::broadcast_category_fade(uint32_t category, float seconds, bool pause_after,
                                                bool unpause) {
    for (auto& [id, s] : sounds) {
        const uint32_t c = s.sound->setup().category;
        if (c == osd::k_none || !category_within(c, category) || s.released) {
            continue;
        }
        if (unpause) {
            // Message 10 ( = 4000), message 6 (resume), then the fade.
            s.sound->set_fade_attenuation(4000);
            s.play_when_loaded = true;
            if (!s.loading) {
                s.sound->resume();
            }
        }
        s.fade = {true,
                  s.sound->fade_attenuation(),
                  category_attenuation(c),
                  std::max(seconds, 0.1f),
                  0.0f,
                  false,
                  pause_after};
    }
}

void AudioSystem::Impl::category_pause(uint32_t category, bool pause, float seconds) {
    if (category == osd::k_none) {
        return;
    }
    categories[category].paused = pause;
    broadcast_category_fade(category, seconds, pause, !pause);
}

void AudioSystem::Impl::category_mute(uint32_t category, bool mute, float seconds) {
    if (category == osd::k_none) {
        return;
    }
    categories[category].muted = mute;
    broadcast_category_fade(category, seconds, false, false);
}

void AudioSystem::Impl::category_set_attenuation(uint32_t category, uint16_t mb, float seconds) {
    if (category == osd::k_none) {
        return;
    }
    categories[category].attenuation = mb;
    broadcast_category_fade(category, seconds, false, false);
}

void AudioSystem::Impl::fade_to_attenuation(uint32_t id, uint16_t mb, float seconds) {
    auto* s = live(id);
    if (!s) {
        return;
    }
    // A newer fade supersedes a pending one; a superseded fade-and-stop becomes an immediate stop
    if (s->fade.active && s->fade.stop_after) {
        stop_sound(id);
        return;
    }
    s->fade = {true, s->sound->fade_attenuation(), mb, std::max(seconds, 0.1f), 0.0f, false, false};
}

void AudioSystem::Impl::fade_out_and_release(uint32_t id, float seconds) {
    auto* s = live(id);
    if (!s) {
        return;
    }
    if (s->loading) {
        s->sound->destroy();
        return;
    }
    s->released = true;
    s->fade = {true, s->sound->fade_attenuation(), 4000, std::max(seconds, 0.1f), 0.0f, true, false};
}

void AudioSystem::Impl::sound_pause(uint32_t id, bool pause) {
    if (auto* s = live(id)) {
        s->play_when_loaded = !pause;
        if (!s->loading) {
            pause ? s->sound->pause() : s->sound->resume();
        }
    }
}

void AudioSystem::set_volume(SoundId id, float volume) { m_impl->sound_set_volume(uint32_t(id), volume, 0.0f); }

void AudioSystem::Impl::sound_set_volume(uint32_t id, float volume, float epsilon) {
    if (auto* s = live(id); s && std::fabs(s->sound->volume() - volume) > epsilon) {
        s->sound->set_volume(volume);
    }
}

void AudioSystem::Impl::sound_set_position(uint32_t id, const glm::vec3& position) {
    if (auto* s = live(id)) {
        s->sound->set_position(position);
        if (!s->loading && device) {
            s->sound->recompute_3d(*device, listener); // SetPosition runs Update3D at once
        }
    }
}

void AudioSystem::Impl::stop_sound(uint32_t id) {
    auto* s = live(id);
    if (!s) {
        return;
    }
    s->released = true;
    if (s->loading) {
        s->sound->destroy();
        return;
    }
    s->sound->stop();
}

void AudioSystem::Impl::handle(const Command& c) {
    switch (c.type) {
    case Command::Type::play: {
        PlayRequest r;
        r.descriptor = c.a;
        r.output_override = c.b;
        if (c.has_position) {
            r.position = c.position;
        }
        start_sound(r, c.id);
        break;
    }
    case Command::Type::play_file: {
        PlayRequest r;
        r.file = owner.m_options.cache / std::string(c.tag);
        r.category = c.a;
        r.output_override = c.b;
        if (c.has_position) {
            r.position = c.position;
        }
        start_sound(r, c.id);
        break;
    }
    case Command::Type::speak: {
        PlayRequest r;
        r.file = owner.m_options.cache / std::string(c.tag);
        if (c.has_position) {
            r.position = c.position;
        }
        r.output_override = c.b;
        if (c.a) {
            r.category = category_by_form(default_object("DDSC"));
            start_sound(r, c.id);
            break;
        }
        const float radius = setting("fMaxHighlightRadius:Audio", 250.0f) * 0.0142875f;
        r.category =
            glm::distance(listener.position, c.position) < radius ? speech_highlight : speech_non_highlight;
        if (start_sound(r, c.id)) {
            speeches.push_back(c.id);
        }
        break;
    }
    case Command::Type::stop:
        stop_sound(c.id);
        break;
    case Command::Type::position:
        sound_set_position(c.id, c.position);
        break;
    case Command::Type::menu_mode: {
        // The game: DOBJ PDMC fades out over fMenuModeFadeOutTime and pauses, PIMC
        // over 100 ms; closing reverses with fMenuModeFadeInTime; the audio thread sleeps
        // uAudioThreadSleepTimeMenuMode.
        const bool open = c.a != 0;
        if (open == menu_mode) {
            break;
        }
        menu_mode = open;
        const float fade =
            open ? setting("fMenuModeFadeOutTime:Audio", 0.5f) : setting("fMenuModeFadeInTime:Audio", 0.5f);
        category_pause(category_by_form(default_object("PDMC")), open, fade);
        category_pause(category_by_form(default_object("PIMC")), open, 0.0f);
        break;
    }
    case Command::Type::loading_menu: {
        const bool open = c.a != 0;
        const float fade =
            open ? setting("fMenuModeFadeOutTime:Audio", 0.5f) : setting("fMenuModeFadeInTime:Audio", 0.5f);
        category_pause(category_by_form(default_object("PDLC")), open, fade);
        break;
    }
    case Command::Type::dialogue: {
        // The game: NDSC and SFDC duck by fNonDialogVoiceDucking dB.
        const bool on = c.a != 0;
        const auto mb = static_cast<uint16_t>(
            on ? std::min(setting("fNonDialogVoiceDucking:Audio", 9.0f) * 100.0f, 10000.0f) : 0.0f);
        const float fade = on ? setting("fNonDialogVoiceDuckingFadeOut:Audio", 1.5f)
                              : setting("fNonDialogVoiceDuckingFadeIn:Audio", 1.0f);
        category_set_attenuation(category_by_form(default_object("NDSC")), mb, fade);
        category_set_attenuation(category_by_form(default_object("SFDC")), mb, fade);
        break;
    }
    case Command::Type::collision:
        ambience->collision(c.a, c.b, c.position, c.f);
        break;
    case Command::Type::listener:
        listener = c.listener;
        break;
    case Command::Type::environment:
        env = c.env;
        break;
    case Command::Type::music_add:
        music->add(c.a);
        break;
    case Command::Type::music_remove:
        music->remove(c.a);
        break;
    case Command::Type::music_stop_all:
        music->stop_all();
        break;
    case Command::Type::animation_sound:
        animation_sound(c);
        break;
    case Command::Type::footstep:
        ambience->footstep(c.a, static_cast<osd::FootstepGait>(c.b), c.tag, c.c, c.position, c.flag, c.flag2);
        break;
    case Command::Type::impact:
        if (c.b) {
            ambience->combat_impact(c.a, c.c, c.position, c.flag, c.flag2);
        } else {
            ambience->impact(c.a, c.c, c.position);
        }
        break;
    case Command::Type::hrtf:
        device->set_hrtf(c.flag);
        for (auto& [id, s] : sounds) {
            s.sound->apply_hrtf(*device, listener);
        }
        break;
    case Command::Type::master_volume:
        break; // read from the atomic
    case Command::Type::category_volume:
        break;
    }
}

void AudioSystem::Impl::tick(float dt) {
    time += dt;
    // The rest of the game's draws from the shared generator, which shape the ones audio makes (spud's
    // only drawer today, so its thread keeps the generator's clock, off the frame loop).
    GameRandom::audio().advance(dt);
    Command c;
    while (commands.pop(c)) {
        handle(c);
    }
    // Menu volumes (atomics written by the main thread).
    for (size_t i = 0; i < menu_volumes.size(); ++i) {
        categories[i].volume = menu_volumes[i].load(std::memory_order_relaxed);
    }
    for (auto& r : loader.take()) {
        if (auto job = cache_loads.find(r.sound); job != cache_loads.end()) {
            // the originally picked file, into the cache without playing.
            if (r.clip && r.error.empty()) {
                cache_add(r.key, job->second, static_cast<uint32_t>(r.clip->pcm.size() * 2), 128);
            }
            cache_loads.erase(job);
            continue;
        }
        auto* s = live(r.sound);
        if (!s) {
            continue;
        }
        s->loading = false;
        if (!r.error.empty()) {
            log::warn("audio: {}", r.error);
            s->sound->destroy();
            continue;
        }
        if (!s->sound->finished()) {
            // The game, when the voice is first created.
            if (r.clip && s->sound->setup().descriptor != osd::k_none &&
                path::to_lower(s->sound->setup().file.extension().string()) == ".wav") {
                cache_add(r.key, s->sound->setup().descriptor, static_cast<uint32_t>(r.clip->pcm.size() * 2),
                          s->sound->setup().priority);
            }
            s->sound->start(*device, std::move(r.clip), std::move(r.stream), s->start_frequency);
            if (!s->play_when_loaded) {
                s->sound->pause();
            }
        }
    }
    update_speech();
    music->update(dt);
    ambience->update(dt);
    const float master = master_volume.load(std::memory_order_relaxed);
    for (auto it = sounds.begin(); it != sounds.end();) {
        auto& s = it->second;
        if (s.fade.active && !s.loading) {
            s.fade.elapsed = std::min(s.fade.elapsed + dt, s.fade.duration);
            const float t = s.fade.elapsed / s.fade.duration;
            const float mb = s.fade.from + (float(s.fade.to) - float(s.fade.from)) * t;
            s.sound->set_fade_attenuation(static_cast<uint16_t>(std::lround(mb)));
            if (s.fade.elapsed >= s.fade.duration) {
                s.fade.active = false;
                s.sound->set_fade_attenuation(s.fade.to);
                if (s.fade.stop_after) {
                    s.sound->stop();
                } else if (s.fade.pause_after) {
                    s.sound->pause();
                }
            }
        }
        if (!s.loading) {
            s.sound->update(*device, listener, master, categories, time);
        }
        if (s.sound->finished()) {
            it = sounds.erase(it);
        } else {
            ++it;
        }
    }
    ++ticks;
    if (ticks % 4 == 0) {
        std::string json = std::format("{{\"available\": true, \"device\": \"{}\", \"sounds\": {}, \"hrtf\": {}, ",
                                       json_escape(device->description()), sounds.size(), device->hrtf());
        json += std::format("\"listener\": [{:.2f}, {:.2f}, {:.2f}], ", listener.position.x,
                            listener.position.y, listener.position.z);
        json += "\"playing\": [";
        bool first = true;
        for (const auto& [id, s] : sounds) {
            const auto descriptor = s.sound->setup().descriptor;
            const std::string name = descriptor != osd::k_none
                                         ? std::string(db.str(db.descriptors[descriptor].edid))
                                         : s.sound->setup().file.filename().string();
            // The fields of the game's sound dump: flags, static, fade and
            // distance attenuation (hundredths of a dB), volume, distance (metres).
            json +=
                std::format("{}{{\"id\": {}, \"sound\": \"{}\", \"gain\": {:.4f}, \"distance\": {:.1f}, "
                            "\"flags\": {}, \"static_mb\": {}, \"attenuation_mb\": {}, \"distance_mb\": {}, "
                            "\"volume\": {:.3f}, \"file\": \"{}\"}}",
                            first ? "" : ", ", id, json_escape(name), s.sound->last_gain(),
                            s.sound->distance(), s.sound->setup().flags, s.sound->setup().static_attenuation,
                            s.sound->fade_attenuation(), s.sound->distance_attenuation(), s.sound->volume(),
                            json_escape(s.sound->setup().file.filename().string()));
            first = false;
        }
        json += "], ";
        json += "\"music\": " + music->state_json() + ", ";
        json += "\"ambience\": " + ambience->state_json() + "}";
        std::lock_guard lock(snapshot_mutex);
        snapshot = std::move(json);
    }
}

void AudioSystem::Impl::run(std::stop_token stop) {
    try {
        device = std::make_unique<Device>(owner.m_options.silent);
    } catch (const std::exception& e) {
        log::error("{}", e.what());
        device_failed = true;
        return;
    }
    device_ready = device.get();
    auto last = Clock::now();
    while (!stop.stop_requested()) {
        const auto now = Clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        tick(dt);
        // work, then sleep uAudioThreadSleepTimeGameMode (MenuMode in menus) ms.
        const char* sleep =
            menu_mode ? "uAudioThreadSleepTimeMenuMode:Audio" : "uAudioThreadSleepTimeGameMode:Audio";
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(setting(sleep, 33.0f))));
    }
    sounds.clear();
    music.reset();
    ambience.reset();
    device.reset();
}

// --- AudioSystem --------------------------------------------------------------------------------------

AudioSystem::AudioSystem(AudioOptions options) : m_options(std::move(options)) {
    const auto db_path = m_options.cache / "audio" / "sound.osd";
    if (!std::filesystem::exists(db_path)) {
        log::warn("audio: {} not found; run skytool import (audio is off)", db_path.string());
        return;
    }
    try {
        m_db = osd::read(read_file(db_path));
    } catch (const std::exception& e) {
        log::error("audio: {}: {}", db_path.string(), e.what());
        return;
    }
    auto index = [&](const auto& items) {
        auto& ix = m_names[&items];
        for (uint32_t i = 0; i < items.size(); ++i) {
            ix.names.emplace(path::to_lower(m_db.str(items[i].edid)), i); // the first of equal names wins
            ix.forms.emplace(items[i].form_id, i);
        }
    };
    index(m_db.descriptors);
    index(m_db.markers);
    index(m_db.music_types);
    index(m_db.categories);
    index(m_db.output_models);
    index(m_db.cells);
    index(m_db.weathers);
    index(m_db.footstep_sets);
    index(m_db.impact_sets);
    index(m_db.materials);
    for (uint32_t i = 0; i < m_db.cells.size(); ++i) {
        const auto& c = m_db.cells[i];
        if (!(c.flags & osd::cell_interior)) {
            m_exterior_cells[(uint64_t(c.world) << 32) ^ (uint64_t(uint16_t(c.x)) << 16) ^ uint16_t(c.y)] = i;
        }
    }
    m_impl = std::make_unique<Impl>(*this);
    m_impl->thread = std::jthread([this](std::stop_token st) { m_impl->run(st); });
    // Opening the device takes a few milliseconds at startup; wait for it so the system is usable at once.
    while (!m_impl->device_ready.load() && !m_impl->device_failed.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (m_impl->device_failed) {
        m_impl.reset();
        return;
    }
    m_available = true;
}

namespace {

std::string menu_key(std::string_view edid) {
    // The Audio menu's vanilla categories under the names the settings screen uses; others by EditorID.
    static const std::pair<std::string_view, std::string_view> names[] = {
        {"audiocategorysfx", "effects"},
        {"audiocategoryfst", "footsteps"},
        {"audiocategoryvocgeneral", "voice"},
        {"audiocategorymus", "music"}};
    const std::string lower = path::to_lower(edid);
    for (const auto& [e, k] : names) {
        if (lower == e) {
            return std::string(k);
        }
    }
    return lower;
}

} // namespace

std::vector<AudioSystem::MenuSlider> AudioSystem::menu_sliders() const {
    std::vector<MenuSlider> out;
    for (size_t i = 0; i < m_db.categories.size(); ++i) {
        const auto& c = m_db.categories[i];
        if (c.flags & osd::category_on_menu) {
            out.push_back({static_cast<uint32_t>(i), c.form_id, menu_key(m_db.str(c.edid)), c.default_menu_value});
        }
    }
    return out;
}

template <typename T>
std::optional<uint32_t> AudioSystem::find_record(const std::vector<T>& items, std::string_view key) const {
    const auto ix = m_names.find(&items);
    if (ix == m_names.end()) {
        return std::nullopt;
    }
    if (auto it = ix->second.names.find(path::to_lower(key)); it != ix->second.names.end()) {
        return it->second;
    }
    if (auto form = parse_form(key)) {
        if (auto it = ix->second.forms.find(*form); it != ix->second.forms.end()) {
            return it->second;
        }
    }
    return std::nullopt;
}

AudioSystem::~AudioSystem() {
    m_impl.reset();
}

std::optional<uint32_t> AudioSystem::find_descriptor(std::string_view s) const {
    if (auto i = find_record(m_db.descriptors, s)) {
        return i;
    }
    // A sound marker (SOUN) names its descriptor.
    if (auto m = find_record(m_db.markers, s); m && m_db.markers[*m].descriptor != osd::k_none) {
        return m_db.markers[*m].descriptor;
    }
    return std::nullopt;
}
std::optional<uint32_t> AudioSystem::find_music_type(std::string_view s) const {
    return find_record(m_db.music_types, s);
}
std::optional<uint32_t> AudioSystem::find_category(std::string_view s) const {
    return find_record(m_db.categories, s);
}
std::optional<uint32_t> AudioSystem::find_output_model(std::string_view s) const {
    return find_record(m_db.output_models, s);
}
std::optional<uint32_t> AudioSystem::find_cell(std::string_view s) const {
    return find_record(m_db.cells, s);
}
std::optional<uint32_t> AudioSystem::find_weather(std::string_view s) const {
    return find_record(m_db.weathers, s);
}
std::optional<uint32_t> AudioSystem::find_footstep_set(std::string_view s) const {
    return find_record(m_db.footstep_sets, s);
}
std::optional<uint32_t> AudioSystem::find_impact_set(std::string_view s) const {
    return find_record(m_db.impact_sets, s);
}
std::optional<uint32_t> AudioSystem::find_material(std::string_view s) const {
    return find_record(m_db.materials, s);
}

AudioSystem::WaterQuery AudioSystem::water_at(uint32_t world, float x, float y) const {
    WaterQuery q;
    const float cell_m = 4096.0f * 0.0142875f;
    const auto cx = static_cast<int32_t>(std::floor(x / cell_m)),
               cy = static_cast<int32_t>(std::floor(y / cell_m));
    auto it = m_exterior_cells.find((uint64_t(world) << 32) ^ (uint64_t(uint16_t(cx)) << 16) ^ uint16_t(cy));
    uint32_t water = osd::k_none;
    if (it != m_exterior_cells.end()) {
        const auto& c = m_db.cells[it->second];
        q.has_water = (c.flags & osd::cell_has_water) != 0;
        if (c.has_water_height) {
            q.height = c.water_height;
        }
        water = c.water;
    }
    if (water == osd::k_none) {
        for (const auto& w : m_db.worlds) {
            if (w.form_id == world) {
                water = w.water;
            }
        }
    }
    if (water != osd::k_none) {
        q.water = m_db.waters[water].form_id;
        const uint32_t sound = m_db.waters[water].sound;
        if (sound != osd::k_none) {
            const uint32_t om = m_db.descriptors[sound].output_model;
            q.radius =
                om != osd::k_none && (m_db.output_models[om].flags & osd::output_attenuates_with_distance)
                    ? m_db.output_models[om].max_distance
                    : 0.0f;
        } else {
            q.water = 0;
        }
    }
    return q;
}

SoundId AudioSystem::play(uint32_t descriptor, std::optional<glm::vec3> position, uint32_t output_model) {
    if (!m_available) {
        return 0;
    }
    Command c;
    c.type = Command::Type::play;
    c.id = m_impl->next_id++;
    c.a = descriptor;
    c.b = output_model;
    c.has_position = position.has_value();
    c.position = position.value_or(glm::vec3(0));
    return m_impl->commands.push(c) ? c.id : 0;
}

SoundId AudioSystem::play_file(std::string_view cache_path, uint32_t category, uint32_t output_model,
                               std::optional<glm::vec3> position) {
    if (!m_available || cache_path.size() >= sizeof(Command::tag)) {
        return 0;
    }
    Command c;
    c.type = Command::Type::play_file;
    c.id = m_impl->next_id++;
    c.a = category;
    c.b = output_model;
    c.has_position = position.has_value();
    c.position = position.value_or(glm::vec3(0));
    cache_path.copy(c.tag, cache_path.size());
    return m_impl->commands.push(c) ? c.id : 0;
}

SoundId AudioSystem::speak(std::string_view cache_path, std::optional<glm::vec3> position, uint32_t output_model,
                           bool in_dialogue) {
    if (!m_available || cache_path.size() >= sizeof(Command::tag)) {
        return 0;
    }
    if (output_model == osd::k_none) {
        // DOBJ DOP2, SOMDialogue3DDefault: the model the game's speech has (measured in the game's
        // lists). DOP3, SOMDialogue2D, is the player's.
        for (const auto& d : m_db.default_objects) {
            if (std::memcmp(d.signature, position ? "DOP2" : "DOP3", 4) == 0) {
                if (auto m = m_names.find(&m_db.output_models); m != m_names.end()) {
                    if (auto it = m->second.forms.find(d.form_id); it != m->second.forms.end()) {
                        output_model = it->second;
                    }
                }
            }
        }
    }
    Command c;
    c.type = Command::Type::speak;
    c.id = m_impl->next_id++;
    c.a = in_dialogue;
    c.b = output_model;
    c.position = position.value_or(glm::vec3(0));
    c.has_position = position.has_value();
    cache_path.copy(c.tag, cache_path.size());
    return m_impl->commands.push(c) ? c.id : 0;
}

uint32_t AudioSystem::player_output_model(bool first_person) const {
    const uint32_t form = m_impl->default_object(first_person ? "POPM" : "P3OM");
    for (uint32_t i = 0; i < m_db.output_models.size(); ++i) {
        if (m_db.output_models[i].form_id == form) {
            return i;
        }
    }
    return osd::k_none;
}

void AudioSystem::stop(SoundId id) {
    if (m_available) {
        Command c;
        c.type = Command::Type::stop;
        c.id = id;
        m_impl->commands.push(c);
    }
}

void AudioSystem::set_position(SoundId id, const glm::vec3& position) {
    if (m_available) {
        Command c;
        c.type = Command::Type::position;
        c.id = id;
        c.position = position;
        m_impl->commands.push(c);
    }
}

void AudioSystem::set_listener(const Listener& listener) {
    if (m_available) {
        Command c;
        c.type = Command::Type::listener;
        c.listener = listener;
        m_impl->commands.push(c);
    }
}

void AudioSystem::set_environment(const Environment& env) {
    if (m_available) {
        Command c;
        c.type = Command::Type::environment;
        c.env = env;
        m_impl->commands.push(c);
    }
}

void AudioSystem::music_add(uint32_t music_type) {
    if (m_available) {
        Command c;
        c.type = Command::Type::music_add;
        c.a = music_type;
        m_impl->commands.push(c);
    }
}

void AudioSystem::music_remove(uint32_t music_type) {
    if (m_available) {
        Command c;
        c.type = Command::Type::music_remove;
        c.a = music_type;
        m_impl->commands.push(c);
    }
}

void AudioSystem::music_stop_all() {
    if (m_available) {
        Command c;
        c.type = Command::Type::music_stop_all;
        m_impl->commands.push(c);
    }
}

void AudioSystem::footstep(uint32_t footstep_set, osd::FootstepGait gait, std::string_view tag,
                           uint32_t material, const glm::vec3& position, bool player, bool first_person) {
    if (m_available) {
        Command c;
        c.type = Command::Type::footstep;
        c.a = footstep_set;
        c.b = static_cast<uint32_t>(gait);
        c.c = material;
        c.position = position;
        c.has_position = true;
        c.flag = player;
        c.flag2 = first_person;
        std::snprintf(c.tag, sizeof(c.tag), "%.*s", static_cast<int>(tag.size()), tag.data());
        m_impl->commands.push(c);
    }
}

void AudioSystem::impact(uint32_t impact_set, uint32_t material, const glm::vec3& position) {
    if (m_available) {
        Command c;
        c.type = Command::Type::impact;
        c.a = impact_set;
        c.c = material;
        c.position = position;
        c.has_position = true;
        m_impl->commands.push(c);
    }
}

void AudioSystem::combat_impact(uint32_t impact_set, uint32_t material, const glm::vec3& position,
                                bool player_attacker, bool normal_hit) {
    if (m_available) {
        Command c;
        c.type = Command::Type::impact;
        c.a = impact_set;
        c.b = 1;
        c.c = material;
        c.position = position;
        c.flag = player_attacker;
        c.flag2 = normal_hit;
        m_impl->commands.push(c);
    }
}

bool AudioSystem::animation_event(uint32_t reference, std::string_view annotation, const glm::vec3& position,
                                  std::optional<glm::vec3> node) {
    // The game splits "Name.Payload" at the first dot and hands
    // the five sound events to the animation sound handler.
    const size_t dot = annotation.find('.');
    const std::string_view name = annotation.substr(0, dot);
    const std::string_view payload =
        dot == std::string_view::npos ? std::string_view{} : annotation.substr(dot + 1);
    using Op = Impl::AnimationSoundOp;
    Op op;
    bool at = false;
    if (path::to_lower(name) == "soundplay") {
        op = Op::play;
    } else if (path::to_lower(name) == "soundplayat") {
        op = Op::play;
        at = true;
    } else if (path::to_lower(name) == "soundstop") {
        op = Op::stop;
    } else if (path::to_lower(name) == "soundrelease") {
        op = Op::release;
    } else if (path::to_lower(name) == "soundanimationend") {
        op = Op::end;
    } else {
        return false;
    }
    if (!m_available || payload.size() >= sizeof(Command::tag)) {
        return true;
    }
    Command c;
    c.type = Command::Type::animation_sound;
    c.a = reference;
    c.b = static_cast<uint32_t>(op);
    c.position = position;
    payload.copy(c.tag, payload.size());
    if (op == Op::play) {
        // The game: a SNDR by editor ID, or a SOUN's descriptor; by name
        // only. SoundPlayAt's payload is "<EDID>.<node>" (split on '.'), and needs both.
        std::string_view edid = payload;
        if (at) {
            const size_t sep = payload.find('.');
            if (sep == std::string_view::npos || sep + 1 >= payload.size() || !node) {
                return true;
            }
            edid = payload.substr(0, sep);
            c.flag = true;
            c.position2 = *node;
        }
        const std::string key = path::to_lower(edid);
        std::optional<uint32_t> d;
        if (auto ix = m_names.find(&m_db.descriptors); ix != m_names.end()) {
            if (auto it = ix->second.names.find(key); it != ix->second.names.end()) {
                d = it->second;
            }
        }
        if (auto ix = m_names.find(&m_db.markers); !d && ix != m_names.end()) {
            if (auto it = ix->second.names.find(key);
                it != ix->second.names.end() && m_db.markers[it->second].descriptor != osd::k_none) {
                d = m_db.markers[it->second].descriptor;
            }
        }
        if (!d) {
            return true;
        }
        c.c = *d;
    }
    m_impl->commands.push(c);
    return true;
}

void AudioSystem::move_animation_sounds(uint32_t reference, const glm::vec3& position) {
    if (m_available) {
        Command c;
        c.type = Command::Type::animation_sound;
        c.a = reference;
        c.b = static_cast<uint32_t>(Impl::AnimationSoundOp::move);
        c.position = position;
        m_impl->commands.push(c);
    }
}

void AudioSystem::collision(uint32_t material_a, uint32_t material_b, const glm::vec3& position,
                            float speed) {
    if (m_available) {
        Command c;
        c.type = Command::Type::collision;
        c.a = material_a;
        c.b = material_b;
        c.position = position;
        c.f = speed;
        m_impl->commands.push(c);
    }
}

uint32_t AudioSystem::material_from_havok_id(uint32_t havok_material) const {
    // CRC32 (reflected, polynomial 0xEDB88320, initial value 0, no final xor) of the
    // lower-cased MNAM; "stone" -> 0xDF02F237.
    for (const auto& m : m_db.materials) {
        uint32_t crc = 0;
        for (char ch : m_db.str(m.name)) {
            crc ^= static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(ch)));
            for (int k = 0; k < 8; ++k) {
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
        }
        if (crc == havok_material) {
            return m.form_id;
        }
    }
    return 0;
}

void AudioSystem::set_menu_mode(bool open) {
    if (m_available) {
        Command c;
        c.type = Command::Type::menu_mode;
        c.a = open;
        m_impl->commands.push(c);
    }
}

void AudioSystem::set_loading_menu(bool open) {
    if (m_available) {
        Command c;
        c.type = Command::Type::loading_menu;
        c.a = open;
        m_impl->commands.push(c);
    }
}

void AudioSystem::set_dialogue(bool open) {
    if (m_available) {
        Command c;
        c.type = Command::Type::dialogue;
        c.a = open;
        m_impl->commands.push(c);
    }
}

void AudioSystem::set_master_volume(float value) {
    if (m_available) {
        m_impl->master_volume = std::clamp(value, 0.0f, 1.0f);
    }
}

void AudioSystem::set_category_volume(uint32_t category, float value) {
    if (m_available && category < m_impl->menu_volumes.size()) {
        m_impl->menu_volumes[category] = std::clamp(value, 0.0f, 1.0f);
    }
}

void AudioSystem::set_hrtf(bool on) {
    if (m_available) {
        Command c;
        c.type = Command::Type::hrtf;
        c.flag = on;
        m_impl->commands.push(c);
    }
}

float AudioSystem::master_volume() const {
    return m_available ? m_impl->master_volume.load() : 0.0f;
}

float AudioSystem::category_volume(uint32_t category) const {
    return m_available && category < m_impl->menu_volumes.size() ? m_impl->menu_volumes[category].load()
                                                                 : 0.0f;
}

bool AudioSystem::capture(const std::filesystem::path& file, double seconds) {
    if (!m_available) {
        return false;
    }
    return m_impl->device_ready.load()->capture().start(file, seconds);
}

bool AudioSystem::capture_busy() const {
    return m_available && m_impl->device_ready.load()->capture().busy();
}

std::string AudioSystem::state_json() const {
    if (!m_available) {
        return "{\"available\": false}";
    }
    if (m_impl->snapshot_mutex.try_lock()) {
        m_impl->snapshot_copy = m_impl->snapshot;
        m_impl->snapshot_mutex.unlock();
    }
    std::string json = m_impl->snapshot_copy.empty() ? "{\"available\": true}" : m_impl->snapshot_copy;
    // The Audio menu values, read here (they live on this thread's side).
    std::string menu = std::format("\"menu\": {{\"master\": {:.3f}", master_volume());
    for (const auto& s : menu_sliders()) {
        menu += std::format(", \"{}\": {:.3f}", s.key, category_volume(s.category));
    }
    json.insert(1, menu + "}, ");
    return json;
}

} // namespace spud::audio
