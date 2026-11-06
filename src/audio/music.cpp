#include "audio/music.hpp"

#include "audio/conditions.hpp"
#include "audio/internal.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <format>
#include <limits>

// Implements (the game /, the game, the three
// The game classes). Statuses: 0 needs a track, 1 playing, 2 paused, 3 finishing, 4 finished.

namespace spud::audio {

namespace {

constexpr int k_no_music = -1; // the built-in NoMusic type: priority 100, no tracks
constexpr uint8_t k_no_music_priority = 100;
constexpr float k_min_type_fade = 4.0f;              // max(WNAM, 4 s)
constexpr uint32_t k_tolerance_ms = 50;              // loop end and cue point tolerance
constexpr uint32_t k_music_category_form = 0x071E64; // DOBJ "Music Sound Category": AudioCategoryMUS
constexpr uint32_t k_music_flags = 0x20;             // creation flags 0xB2: 0x20 = XAudio2 music voice
constexpr uint32_t k_pending_removal = 0x10;

enum Status : uint8_t { s_inactive = 0, s_playing = 1, s_paused = 2, s_finishing = 3, s_finished = 4 };

} // namespace

struct MusicManager::State {
    struct Track {
        Status status = s_inactive;
        // single
        uint32_t main = 0, finale = 0;
        uint32_t next_cue = 0;
        uint32_t loops_left = 0;
        uint32_t last_pos_ms = std::numeric_limits<uint32_t>::max();
        bool cue_wait = false; // finishing toward a cue point
        // palette (layers of sub-track indices) and silent
        std::vector<uint32_t> layers[3];
        int current[3] = {-1, -1, -1};
        int previous[3] = {-1, -1, -1};
        double start = 0.0, paused_at = 0.0;
        bool finish_requested = false;
    };
    struct Type {
        Status status = s_inactive;
        uint32_t flags = 0;
        int current_track = -1; // index into the type's track list
        int previous_pick = 0;  // starts at 0: the first random pick can never be track 0
        std::vector<int> order; // cycle order list, shuffled once per session
    };
    std::vector<Track> tracks;
    std::vector<Type> types;
    std::vector<int> heap{k_no_music};
    int current = k_no_music;
    Status no_music_status = s_inactive;
    int pause_count = 0;
    int cell_music = k_no_music; // The game's stored type
    uint32_t music_category = osd::k_none;
};

MusicManager::MusicManager(AudioSystem::Impl& audio) : m_audio(audio), m_state(std::make_unique<State>()) {
    auto& st = *m_state;
    const auto& db = audio.db;
    st.tracks.resize(db.music_tracks.size());
    st.types.resize(db.music_types.size());
    for (size_t i = 0; i < db.music_types.size(); ++i) {
        st.types[i].flags = db.music_types[i].flags;
    }
    for (size_t i = 0; i < db.music_tracks.size(); ++i) {
        const auto& t = db.music_tracks[i];
        if (t.type == osd::TrackType::palette) {
            // SNAM splits into up to three layers at each empty entry.
            int layer = 0;
            for (uint32_t k = 0; k < t.sub_count; ++k) {
                const uint32_t sub = db.track_lists[t.first_sub + k];
                if (sub == osd::k_none) {
                    layer = std::min(layer + 1, 2);
                    continue;
                }
                st.tracks[i].layers[layer].push_back(sub);
            }
        }
    }
    for (size_t i = 0; i < db.categories.size(); ++i) {
        if (db.categories[i].form_id == k_music_category_form) {
            st.music_category = static_cast<uint32_t>(i);
        }
    }
}

MusicManager::~MusicManager() = default;

// --- helpers -------------------------------------------------------------------------------------------

namespace {

uint8_t priority_of(const osd::SoundDb& db, int type) {
    return type == k_no_music ? k_no_music_priority : static_cast<uint8_t>(db.music_types[type].priority);
}

} // namespace

struct MusicOps {
    MusicManager::State& st;
    AudioSystem::Impl& audio;
    const osd::SoundDb& db = audio.db;

    float setting(const char* name, float fallback) const { return audio.setting(name, fallback); }

    ConditionContext context() const { return audio.condition_context(); }

    bool can_play(uint32_t track) const {
        const auto& t = db.music_tracks[track];
        return evaluate(std::span(db.conditions).subspan(t.first_condition, t.condition_count), context());
    }

    Status& status(int type) { return type == k_no_music ? st.no_music_status : st.types[type].status; }
    uint32_t flags(int type) const { return type == k_no_music ? 0 : st.types[type].flags; }

    // --- tracks ---

    uint32_t play_file(uint32_t file_string, float fade_in) {
        if (file_string == container::k_no_string) {
            return 0;
        }
        PlayRequest r;
        r.file = audio.file_path(file_string);
        r.category = st.music_category;
        r.priority = 0x40;
        r.flags = k_music_flags;
        r.fade_in = fade_in;
        return audio.start_sound(r);
    }

    void track_play(uint32_t ti) {
        auto& t = st.tracks[ti];
        const auto& rec = db.music_tracks[ti];
        switch (rec.type) {
        case osd::TrackType::single:
            if (t.status == s_paused && audio.sound_valid(t.main)) {
                audio.sound_pause(t.main, false);
            } else if (!audio.sound_valid(t.main)) {
                t.main = play_file(rec.file, 0.0f);
            }
            // Every DoPlay resets the loop count, a resume included.
            if (rec.has_loop && rec.loop_end > rec.loop_begin && rec.loop_end > 0) {
                t.loops_left = rec.loop_count;
            }
            t.last_pos_ms = std::numeric_limits<uint32_t>::max();
            t.status = s_playing;
            break;
        case osd::TrackType::palette:
            if (t.status == s_paused) {
                t.start += audio.time - t.paused_at;
                for (int l = 0; l < 3; ++l) {
                    if (t.current[l] >= 0) {
                        track_play(t.layers[l][t.current[l]]);
                    }
                }
            } else {
                for (int l = 0; l < 3; ++l) {
                    palette_pick(t, l);
                }
                t.start = audio.time;
                t.finish_requested = false;
            }
            t.status = s_playing;
            break;
        case osd::TrackType::silent:
            if (t.status == s_paused) {
                t.start += audio.time - t.paused_at;
            } else {
                t.start = audio.time;
            }
            t.status = s_playing;
            break;
        }
    }

    void palette_pick(MusicManager::State::Track& t, int layer) {
        const auto& list = t.layers[layer];
        t.current[layer] = -1;
        std::vector<int> ok;
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            if (list[i] != osd::k_none && (list.size() == 1 || i != t.previous[layer]) && can_play(list[i])) {
                ok.push_back(i);
            }
        }
        if (ok.empty()) {
            return;
        }
        const int pick = ok[audio.random_below(static_cast<uint32_t>(ok.size()))];
        t.current[layer] = pick;
        t.previous[layer] = pick;
        st.tracks[list[pick]].status = s_inactive;
        track_play(list[pick]);
    }

    void track_update(uint32_t ti) {
        auto& t = st.tracks[ti];
        const auto& rec = db.music_tracks[ti];
        switch (rec.type) {
        case osd::TrackType::single: {
            const bool main_valid = audio.sound_valid(t.main);
            if (t.status == s_playing && !main_valid && !audio.sound_valid(t.finale)) {
                t.status = s_finished;
                return;
            }
            if (t.status == s_finishing && !main_valid) {
                t.status = s_finished; // the finale, if any, plays on by itself
                return;
            }
            if (!main_valid) {
                return;
            }
            const uint32_t pos = audio.sound_position_ms(t.main);
            if (t.status == s_playing && t.loops_left > 0 && rec.has_loop && pos != t.last_pos_ms) {
                const auto end_ms = static_cast<uint32_t>(rec.loop_end * 1000.0f);
                if (pos + k_tolerance_ms >= end_ms) {
                    audio.sound_seek_ms(t.main, static_cast<uint32_t>(rec.loop_begin * 1000.0f));
                    --t.loops_left;
                }
            }
            if (t.status == s_finishing && t.cue_wait && t.next_cue < rec.cue_count) {
                const auto cue_ms = static_cast<uint32_t>(cue(rec, t.next_cue) * 1000.0f);
                if (pos + k_tolerance_ms >= cue_ms) {
                    t.cue_wait = false;
                    if (rec.finale != container::k_no_string) {
                        audio.fade_out_and_release(t.main,
                                                   setting("fMusicFinaleCrossFadeTimeOut:Audio", 6.0f));
                        t.finale = play_file(rec.finale, setting("fMusicFinaleCrossFadeTimeIn:Audio", 3.0f));
                    } else {
                        audio.sound_seek_ms(t.main,
                                            static_cast<uint32_t>(cue(rec, rec.cue_count - 1) * 1000.0f));
                    }
                }
            }
            t.last_pos_ms = pos;
            break;
        }
        case osd::TrackType::palette: {
            if (t.status != s_playing && t.status != s_finishing) {
                return;
            }
            for (int l = 0; l < 3; ++l) {
                if (t.current[l] >= 0) {
                    const uint32_t sub = t.layers[l][t.current[l]];
                    track_update(sub);
                    if (st.tracks[sub].status == s_finished && t.status == s_playing) {
                        palette_pick(t, l);
                    }
                } else if (t.status == s_playing && !t.layers[l].empty()) {
                    palette_pick(t, l);
                }
            }
            const double elapsed = audio.time - t.start;
            if (t.status == s_playing && elapsed > rec.duration - std::min(rec.fade_out, rec.duration)) {
                track_finish(ti, false, std::min(rec.fade_out, rec.duration));
            } else if (t.status == s_finishing && elapsed > rec.duration) {
                t.status = s_finished;
            }
            break;
        }
        case osd::TrackType::silent:
            if (t.status == s_playing && audio.time - t.start > rec.duration) {
                t.status = s_finished;
            }
            break;
        }
    }

    float cue(const osd::MusicTrack& rec, uint32_t i) const {
        // Cue points are sorted at load; the importer keeps file order.
        std::vector<float> cues(db.cue_points.begin() + rec.first_cue,
                                db.cue_points.begin() + rec.first_cue + rec.cue_count);
        std::sort(cues.begin(), cues.end());
        return cues[i];
    }

    void track_finish(uint32_t ti, bool immediate, float fade) {
        auto& t = st.tracks[ti];
        const auto& rec = db.music_tracks[ti];
        switch (rec.type) {
        case osd::TrackType::single: {
            if (rec.cue_count == 0 || immediate || t.status == s_paused) {
                t.status = s_finished;
                audio.fade_out_and_release(t.main, immediate ? 0.5f : fade);
                audio.fade_out_and_release(t.finale, immediate ? 0.5f : fade);
                return;
            }
            t.status = s_finishing;
            const uint32_t pos = audio.sound_position_ms(t.main);
            t.next_cue = 0;
            while (t.next_cue < rec.cue_count && cue(rec, t.next_cue) * 1000.0f <= pos) {
                ++t.next_cue;
            }
            // Past the last cue point the track plays out to its natural end.
            t.cue_wait = t.next_cue < rec.cue_count;
            if (!t.cue_wait) {
                t.next_cue = 0;
            }
            break;
        }
        case osd::TrackType::palette:
            for (int l = 0; l < 3; ++l) {
                if (t.current[l] >= 0) {
                    track_finish(t.layers[l][t.current[l]], immediate, fade);
                }
            }
            t.status = s_finishing;
            break;
        case osd::TrackType::silent:
            t.status = s_finished;
            break;
        }
    }

    void track_pause(uint32_t ti) {
        auto& t = st.tracks[ti];
        const auto& rec = db.music_tracks[ti];
        switch (rec.type) {
        case osd::TrackType::single:
            if (t.status == s_finishing) {
                t.status = s_finished;
                audio.fade_out_and_release(t.main, 0.5f);
                audio.fade_out_and_release(t.finale, 0.5f);
            } else {
                audio.sound_pause(t.main, true);
                t.status = s_paused;
            }
            break;
        case osd::TrackType::palette:
            for (int l = 0; l < 3; ++l) {
                if (t.current[l] >= 0) {
                    track_pause(t.layers[l][t.current[l]]);
                }
            }
            t.paused_at = audio.time;
            t.status = s_paused;
            break;
        case osd::TrackType::silent:
            t.paused_at = audio.time;
            t.status = s_paused;
            break;
        }
    }

    // Only single tracks duck (palette and silent ducking are no-ops).
    void track_duck(uint32_t ti, uint16_t mb) {
        if (db.music_tracks[ti].type == osd::TrackType::single) {
            audio.fade_to_attenuation(st.tracks[ti].main, mb, setting("fMusicDuckingSeconds:Audio", 1.0f));
        }
    }
    void track_unduck(uint32_t ti) {
        if (db.music_tracks[ti].type == osd::TrackType::single) {
            audio.fade_to_attenuation(st.tracks[ti].main, 0, setting("fMusicUnDuckingSeconds:Audio", 4.0f));
        }
    }

    // --- types ---

    uint32_t track_of(int type, int index) const {
        const auto& m = db.music_types[type];
        return db.track_lists[m.first_track + index];
    }

    void type_prepare(int type) {
        if (type == k_no_music) {
            st.no_music_status = s_inactive;
            return;
        }
        auto& t = st.types[type];
        const auto& m = db.music_types[type];
        if ((t.flags & osd::music_cycle_tracks) && t.order.empty()) {
            for (uint32_t i = 0; i < m.track_count; ++i) {
                t.order.push_back(static_cast<int>(i));
            }
            if (!(t.flags & osd::music_maintain_track_order)) {
                for (size_t i = t.order.size(); i > 1; --i) { // Fisher-Yates
                    std::swap(t.order[i - 1], t.order[audio.random_below(static_cast<uint32_t>(i))]);
                }
            }
        }
        if (t.status != s_playing) {
            t.status = s_inactive;
        }
    }

    void type_play(int type) {
        if (type == k_no_music) {
            st.no_music_status = s_playing;
            return;
        }
        auto& t = st.types[type];
        const auto& m = db.music_types[type];
        if (t.status == s_paused && t.current_track >= 0) {
            track_play(track_of(type, t.current_track));
            t.status = s_playing;
            return;
        }
        std::vector<int> playable;
        for (uint32_t i = 0; i < m.track_count; ++i) {
            const uint32_t tr = track_of(type, static_cast<int>(i));
            if (tr != osd::k_none && can_play(tr)) {
                playable.push_back(static_cast<int>(i));
            }
        }
        int pick = -1;
        if (t.flags & osd::music_cycle_tracks) {
            for (size_t k = 0; k < t.order.size(); ++k) {
                if (std::find(playable.begin(), playable.end(), t.order[k]) != playable.end()) {
                    pick = t.order[k];
                    t.order.erase(t.order.begin() + static_cast<std::ptrdiff_t>(k));
                    t.order.push_back(pick);
                    break;
                }
            }
        } else {
            std::vector<int> candidates;
            for (int i : playable) {
                if (m.track_count <= 1 || i != t.previous_pick) {
                    candidates.push_back(i);
                }
            }
            if (!candidates.empty()) {
                pick = candidates[audio.random_below(static_cast<uint32_t>(candidates.size()))];
                t.previous_pick = pick;
            } else if (t.current_track >= 0 && can_play(track_of(type, t.current_track))) {
                pick = t.current_track;
            }
        }
        if (pick < 0) {
            t.status = s_finished;
            return;
        }
        t.current_track = pick;
        const uint32_t tr = track_of(type, pick);
        st.tracks[tr].status = s_inactive;
        track_play(tr);
        t.status = s_playing;
    }

    void type_update(int type) {
        if (type == k_no_music) {
            return;
        }
        auto& t = st.types[type];
        if (t.current_track < 0 || (t.status != s_playing && t.status != s_finishing)) {
            return;
        }
        const uint32_t tr = track_of(type, t.current_track);
        if (st.tracks[tr].status == s_finished) {
            if (t.status == s_finishing) {
                t.status = s_finished;
            } else if (t.flags & osd::music_plays_one_selection) {
                t.flags |= k_pending_removal;
                t.status = s_finished;
            } else {
                t.status = s_inactive;
            }
            return;
        }
        track_update(tr);
        if (t.status == s_finishing && st.tracks[tr].status == s_finished) {
            t.status = s_finished;
        }
    }

    void type_finish(int type) {
        if (type == k_no_music) {
            st.no_music_status = s_finished;
            return;
        }
        auto& t = st.types[type];
        // The "immediate" argument is ignored: always max(WNAM, 4 s).
        const float fade = std::max(db.music_types[type].fade_duration, k_min_type_fade);
        if (t.current_track >= 0 && t.status != s_inactive) {
            const uint32_t tr = track_of(type, t.current_track);
            track_finish(tr, false, fade);
            t.status = st.tracks[tr].status == s_finished ? s_finished : s_finishing;
        } else {
            t.status = s_finished;
        }
    }

    void type_pause(int type) {
        if (type == k_no_music) {
            st.no_music_status = s_paused;
            return;
        }
        auto& t = st.types[type];
        if (t.current_track >= 0) {
            track_pause(track_of(type, t.current_track));
        }
        t.status = s_paused;
    }

    void type_duck(int type, uint16_t mb) {
        if (type != k_no_music && st.types[type].current_track >= 0) {
            track_duck(track_of(type, st.types[type].current_track), mb);
        }
    }
    void type_unduck(int type) {
        if (type != k_no_music && st.types[type].current_track >= 0) {
            track_unduck(track_of(type, st.types[type].current_track));
        }
    }

    // --- the heap: lowest priority byte on top; equal priorities keep the earlier entry above ---

    void heap_push(int type) {
        auto& h = st.heap;
        h.push_back(type);
        size_t i = h.size() - 1;
        while (i > 0) {
            const size_t parent = (i - 1) / 2;
            if (priority_of(db, h[parent]) <= priority_of(db, h[i])) {
                break;
            }
            std::swap(h[parent], h[i]);
            i = parent;
        }
    }

    void heap_remove(int type) {
        auto& h = st.heap;
        auto it = std::find(h.begin(), h.end(), type);
        if (it == h.end()) {
            return;
        }
        size_t i = static_cast<size_t>(it - h.begin());
        std::swap(h[i], h.back());
        h.pop_back();
        // Sift down only (HeapRemove).
        for (;;) {
            const size_t l = 2 * i + 1, r = l + 1;
            size_t best = i;
            if (l < h.size() && priority_of(db, h[l]) < priority_of(db, h[best])) {
                best = l;
            }
            if (r < h.size() && priority_of(db, h[r]) < priority_of(db, h[best])) {
                best = r;
            }
            if (best == i) {
                break;
            }
            std::swap(h[i], h[best]);
            i = best;
        }
    }

    bool in_heap(int type) const { return std::find(st.heap.begin(), st.heap.end(), type) != st.heap.end(); }
};

void MusicManager::add(uint32_t music_type) {
    MusicOps ops{*m_state, m_audio};
    auto& st = *m_state;
    const int type = static_cast<int>(music_type);
    if (music_type >= st.types.size()) {
        return;
    }
    auto& t = st.types[type];
    if (ops.in_heap(type) && (t.flags & k_pending_removal)) {
        t.flags &= ~k_pending_removal;
        if (t.status == s_finishing || t.status == s_finished) {
            ops.type_finish(type);
            ops.type_prepare(type);
        }
        return;
    }
    if (t.flags & osd::music_doesnt_queue) {
        if (!st.heap.empty() && !(priority_of(m_audio.db, type) < priority_of(m_audio.db, st.heap.front()))) {
            return;
        }
    }
    ops.heap_push(type);
}

void MusicManager::remove(uint32_t music_type) {
    MusicOps ops{*m_state, m_audio};
    auto& st = *m_state;
    const int type = static_cast<int>(music_type);
    if (music_type >= st.types.size() || !ops.in_heap(type)) {
        return;
    }
    if (type == st.current) {
        st.types[type].flags |= k_pending_removal;
        ops.type_finish(type);
    } else {
        ops.type_finish(type);
        ops.heap_remove(type);
    }
}

void MusicManager::stop_all() {
    MusicOps ops{*m_state, m_audio};
    auto& st = *m_state;
    for (int t : std::vector<int>(st.heap.begin(), st.heap.end())) {
        if (t != k_no_music) {
            remove(static_cast<uint32_t>(t));
        }
    }
    st.cell_music = k_no_music;
}

void MusicManager::set_cell_music(uint32_t music_type) {
    // on a cell change to a different type, remove the old one, add the new one.
    auto& st = *m_state;
    const int type = music_type == osd::k_none ? k_no_music : static_cast<int>(music_type);
    if (type == st.cell_music) {
        return;
    }
    if (st.cell_music != k_no_music) {
        remove(static_cast<uint32_t>(st.cell_music));
    }
    st.cell_music = type;
    if (type != k_no_music) {
        add(static_cast<uint32_t>(type));
    }
}

void MusicManager::update(float) {
    MusicOps ops{*m_state, m_audio};
    auto& st = *m_state;
    const int top = st.heap.front();
    if (top == st.current) {
        if (ops.status(st.current) == s_finished && (ops.flags(st.current) & k_pending_removal)) {
            st.types[st.current].flags &= ~k_pending_removal;
            ops.heap_remove(st.current);
            st.current = k_no_music;
        } else if (ops.status(st.current) == s_inactive) {
            ops.type_play(st.current);
        }
    } else {
        bool switch_now = false;
        if (ops.flags(top) & osd::music_ducks_current_track) {
            ops.type_duck(st.current, m_audio.db.music_types[top].ducking);
            switch_now = true;
        } else {
            const auto s = ops.status(st.current);
            if (s != s_finishing && s != s_finished) {
                ops.type_finish(st.current);
            }
            switch_now = ops.status(st.current) == s_finished;
        }
        if (switch_now) {
            const int old = st.current;
            const Status old_status = ops.status(old);
            if (old != k_no_music && (st.types[old].flags & k_pending_removal)) {
                st.types[old].flags &= ~k_pending_removal;
                ops.heap_remove(old);
            }
            st.current = top;
            if (ops.status(top) != s_playing) {
                ops.type_prepare(top);
                ops.type_play(top);
            }
            ops.type_unduck(top);
            if (old_status == s_paused) {
                ops.type_pause(top);
            }
        }
    }
    ops.type_update(st.current);
}

std::string MusicManager::state_json() const {
    const auto& st = *m_state;
    const auto& db = m_audio.db;
    auto type_name = [&](int t) {
        return t == k_no_music ? std::string("NoMusic") : std::string(db.str(db.music_types[t].edid));
    };
    std::string heap;
    for (size_t i = 0; i < st.heap.size(); ++i) {
        heap += std::format("{}\"{}\"", i ? ", " : "", type_name(st.heap[i]));
    }
    std::string track = "null";
    if (st.current != k_no_music && st.types[st.current].current_track >= 0) {
        const uint32_t tr =
            db.track_lists[db.music_types[st.current].first_track + st.types[st.current].current_track];
        if (tr != osd::k_none) {
            const auto& t = st.tracks[tr];
            track = std::format("{{\"edid\": \"{}\", \"status\": {}, \"position_ms\": {}}}",
                                db.str(db.music_tracks[tr].edid), int(t.status),
                                m_audio.sound_position_ms(t.main));
        }
    }
    return std::format(
        "{{\"current\": \"{}\", \"status\": {}, \"cell_music\": \"{}\", \"heap\": [{}], \"track\": {}}}",
        type_name(st.current),
        st.current == k_no_music ? int(st.no_music_status) : int(st.types[st.current].status),
        type_name(st.cell_music), heap, track);
}

} // namespace spud::audio
