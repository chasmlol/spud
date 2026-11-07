// Saved games: gathering the systems' change layers, writing them off the frame, and putting a game back

#include "engine/app_internal.hpp"

#include <SDL3/SDL.h>

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/platform.hpp"
#include "core/units.hpp"
#include "formats/forms.hpp"
#include "game/game.hpp"
#include "game/quests.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <ctime>
#include <format>
#include <utility>

namespace spud::engine {

namespace {

constexpr uint32_t k_thumb_width = 320, k_thumb_height = 192;
constexpr int k_capture_frames = 3; // frames in flight: the captured one is done by then, and nothing waits for it
constexpr int k_tween_delay = 6;     // uTweenSaveDelay:Interface
constexpr int k_after_load_passes = 10; // what the save queue skips once a loading screen has gone
constexpr float k_death_reload = 5.0f;  // fPlayerDeathReloadTime, Skyrim.esm
constexpr float k_health_limit = 0.25f; // fPlayerHealthSaveOnPauseLimit:GamePlay

// In a file name: `, tab \ / : * ; = < > ? |` and anything past ASCII become a space, `"` a `'`.
std::string file_safe(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c == '"') {
            c = '\'';
        } else if (static_cast<unsigned char>(c) >= 0x80 || std::string_view(",\t\\/:*;=<>?|").find(c) != std::string_view::npos) {
            c = ' ';
        }
    }
    return out;
}

} // namespace

std::filesystem::path App::saves_dir() const {
    if (!m_opt.saves.empty()) {
        return m_opt.saves;
    }
    return platform::documents() / "My Games" / "spud" / "Saves";
}

bool App::modded() {
    // Anything past the official plugins: the five masters and the Creation Club's. The load order can't change under a
    // running game, so it's worked out once.
    if (!m_modded && m_game) {
        m_modded = false;
        const forms::Database& db = m_game->db();
        for (size_t i = 0; i < db.plugins().size(); ++i) {
            const std::string name = path::to_lower(db.plugin_name(i));
            const bool official = name == "skyrim.esm" || name == "update.esm" || name == "dawnguard.esm" ||
                                  name == "hearthfires.esm" || name == "dragonborn.esm" || name.starts_with("cc");
            m_modded = *m_modded || !official;
        }
    }
    return m_modded.value_or(false);
}

App::SaveRecord& App::save_record() {
    if (!m_character_id) {
        // A view straight into a cell never went through New Game.
        m_character_id = uint32_t(std::chrono::system_clock::now().time_since_epoch().count());
        m_save_character = m_character_id;
    }
    return m_save_records[uint64_t(m_character_id) << 1 | (modded() ? 1 : 0)];
}

std::string App::save_stem(std::string_view prefix, uint32_t n) {
    // <Prefix><N>_<CharId>_<Modded>_<NameHex>_<LocationEdid>_<PlayTime>_<Date>_<Level>_1.
    std::string name_hex;
    for (const char c : m_game ? m_game->player().name : std::string()) {
        name_hex += std::format("{:02X}", static_cast<unsigned char>(c));
    }
    int32_t cx = 0, cy = 0;
    const WorldSim::Place place = m_world_sim ? player_place(cx, cy) : WorldSim::Place{};
    const std::string location = m_world_sim ? m_world_sim->place_edid(place) : m_space->edid();
    const auto played = int64_t(m_play_seconds);
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &now);
    std::string stem = std::format("{}{}_{:08X}_{}_{}_{}_{:02}{:02}{:02}_{:04}{:02}{:02}{:02}{:02}{:02}_{}_1", prefix, n,
                                   m_character_id, modded() ? 1 : 0, name_hex, location, played / 3600, played / 60 % 60,
                                   played % 60, utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
                                   utc.tm_sec, m_game ? m_game->character().level() : 1);
    return file_safe(stem).substr(0, 250);
}

bool App::can_save() {
    // CanSaveGame: one test, one message. VATS, mounts, paired animations and the furniture camera join it
    // when those exist.
    return m_space && !m_saving_disabled && !player_dead() && !m_loading && (!m_ui || m_ui->menus_allow_saving());
}

bool App::can_load() {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(saves_dir(), ec)) {
        if (path::to_lower(e.path().extension().string()) == ".ost") {
            return true;
        }
    }
    return false;
}

bool App::player_dead() const { return m_walk && m_player && m_game && m_game->combat().fighter(game::k_player_ref).dead(); }

void App::notify_cant_save() {
    if (m_ui) {
        m_ui->hud_corner_message(m_ui->game_setting("sCantSaveNow"));
    }
}

void App::manual_save(const std::filesystem::path& replaces) {
    if (!can_save()) {
        notify_cant_save();
        return;
    }
    start_game();
    SaveRecord& r = save_record();
    begin_save(ost::Kind::manual, save_stem("Save", r.next_number), r.next_number, replaces);
    ++r.next_number; // a finished manual save moves it on; every save's header holds the next one
}

void App::autosave() {
    // Refused silently, unlike the others. A view straight into a cell is no game to save: only New Game or a load make a
    // character, so scripted runs and benches never write one by walking through a door.
    if (!can_save() || !m_character_id) {
        return;
    }
    start_game();
    SaveRecord& r = save_record();
    const uint32_t count = m_rules ? uint32_t(m_rules->setting("iAutoSaveCount").value_or(3.0)) : 3;
    const uint32_t slot = r.autosave_slot;
    r.autosave_slot = slot % std::max(count, 1u) + 1; // 1, 2, 3, 1
    std::filesystem::path old;
    if (const auto it = r.slots.find(slot); it != r.slots.end()) {
        old = it->second;
    }
    begin_save(ost::Kind::autosave, save_stem("Autosave", slot), r.next_number, old);
    r.slots[slot] = m_pending_save->path;
    if (m_ui) {
        m_ui->hud_corner_message(m_ui->game_setting("sAutoSaving"));
    }
}

void App::quick_save() {
    if (!can_save()) {
        notify_cant_save();
        return;
    }
    start_game();
    SaveRecord& r = save_record();
    const std::filesystem::path old = r.quicksave;
    begin_save(ost::Kind::quicksave, save_stem("Quicksave", 0), r.next_number, old);
    r.quicksave = m_pending_save->path;
    if (m_ui) {
        m_ui->hud_corner_message(m_ui->game_setting("sQuickSaving"));
    }
}

void App::ui_quicksave_key() {
    if (can_save()) {
        quick_save();
    }
}

void App::ui_quickload_key() {
    // The current character's quicksave only, with no question.
    const std::filesystem::path file = save_record().quicksave;
    std::error_code ec;
    if (file.empty() || !std::filesystem::exists(file, ec)) {
        if (m_ui) {
            m_ui->hud_corner_message(m_ui->game_setting("sSaveGameCorruptMenuMessage"));
        }
        return;
    }
    if (m_ui) {
        m_ui->hud_corner_message(m_ui->game_setting("sQuickLoading"));
    }
    m_load_request = file;
}

void App::ui_autosave(const char* occasion) {
    const std::string option = occasion == std::string_view("rest") ? "bSaveOnRest:SaveGame" : "bSaveOnWait:SaveGame";
    if (m_user.number(option) > 0.5) {
        autosave();
    }
}

void App::ui_tween_opened() {
    // the timer has no tick of its own, it is read as this menu opens. Real time since the last finished save.
    const double minutes = m_user.number("fAutosaveEveryXMins:SaveGame");
    const double since = std::chrono::duration<double>(Clock::now() - m_last_save_time).count();
    if (m_user.number("bSaveOnPause:SaveGame") < 0.5 || since <= minutes * 60.0) {
        return;
    }
    const float health = m_walk && m_player ? m_player->status().health / std::max(m_tuning.max_health, 1.0f) : 1.0f;
    if (health > k_health_limit) {
        m_autosave_in = k_tween_delay;
        if (m_ui) {
            m_ui->hud_corner_message(m_ui->game_setting("sAutoSaving"));
        }
    }
}

void App::begin_save(ost::Kind kind, const std::string& stem, uint32_t number, const std::filesystem::path& replaces) {
    if (!m_space) {
        return;
    }
    const auto started = Clock::now();
    finish_save_write(); // the one before, normally long done
    auto save = std::make_unique<PendingSave>();
    save->path = saves_dir() / (stem + ".ost");
    save->replaces = replaces;
    ost::Save& s = save->save;
    s.header.kind = kind;
    s.header.number = number;
    s.header.character = m_character_id;
    s.header.modded = modded() ? 1 : 0;
    s.header.level = m_game ? uint32_t(m_game->character().level()) : 1;
    s.header.days_passed = m_clock.days_passed();
    s.header.play_seconds = m_play_seconds;
    s.header.saved_at = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
    s.player_name = m_game ? m_game->player().name : std::string();
    s.location = save_location_text();
    s.race = save_race_text();
    s.space = m_space->edid();
    s.plugins = save_plugins();
    uint64_t order = 0;
    for (const ost::Plugin& p : s.plugins) {
        order = order * 0x9E3779B97F4A7C15ull ^ p.hash;
    }
    s.header.load_order = order;

    BinaryWriter clock;
    m_clock.save(clock);
    s.sections.emplace_back(ost::k_clock, clock.take());
    if (m_world_sim) {
        BinaryWriter world;
        m_world_sim->save(world);
        s.sections.emplace_back(ost::k_world, world.take());
    }
    BinaryWriter player;
    const glm::vec3 feet = player_position_units() * units::k_skyrim_unit_m;
    player.write<float>(feet.x);
    player.write<float>(feet.y);
    player.write<float>(feet.z);
    player.write<float>(m_camera.yaw);
    player.write<float>(m_camera.pitch);
    player.write<uint8_t>(m_walk ? 1 : 0);
    player.write<uint8_t>(m_custom_marker ? 1 : 0);
    const glm::vec3 custom = m_custom_marker.value_or(glm::vec3(0.0f));
    player.write<float>(custom.x);
    player.write<float>(custom.y);
    player.write<float>(custom.z);
    player.write<float>(m_last_exterior.x);
    player.write<float>(m_last_exterior.y);
    player.write<float>(m_last_exterior.z);
    player.write<uint32_t>(m_last_exterior_world);
    player.write<uint8_t>(m_saving_disabled ? 1 : 0);
    s.sections.emplace_back(ost::k_player, player.take());
    if (m_game) {
        s.sections.emplace_back(ost::k_game, m_game->save());
        s.sections.emplace_back(ost::k_actors, save_actors());
    }
    if (m_actor_data) {
        BinaryWriter gear;
        const auto all = ost::sorted(m_actor_data->npc_gear());
        gear.write<uint32_t>(uint32_t(all.size()));
        for (const auto& [ref, g] : all) {
            gear.write<uint32_t>(ref);
            gear.write<uint8_t>((g.bare ? 1 : 0) | (g.headless ? 2 : 0));
            for (const std::vector<uint32_t>* list : {&g.on, &g.off}) {
                gear.write<uint32_t>(uint32_t(list->size()));
                gear.write_span(std::span<const uint32_t>(*list));
            }
        }
        s.sections.emplace_back(ost::k_npc_gear, gear.take());
    }
    if (m_sky) {
        BinaryWriter sky;
        sky.write<uint32_t>(m_sky->weather());
        s.sections.emplace_back(ost::k_sky, sky.take());
    }
    if (m_papyrus) {
        s.sections.emplace_back(ost::k_papyrus, m_papyrus->vm().save());
        if (game::QuestSystem* quests = m_papyrus->quests()) {
            s.sections.emplace_back(ost::k_quests, quests->save());
        }
    }
    if (m_ui) {
        BinaryWriter ui;
        const std::vector<int> shown = m_ui->tutorials_shown_list();
        ui.write<uint32_t>(uint32_t(shown.size()));
        ui.write_span(std::span<const int>(shown));
        s.sections.emplace_back(ost::k_ui, ui.take());
    }
    if (m_destruction) {
        BinaryWriter destruction;
        m_destruction->save(destruction);
        s.sections.emplace_back(ost::k_destruction, destruction.take());
    }
    if (m_local_map) {
        BinaryWriter seen;
        m_local_map->seen().save(seen);
        s.sections.emplace_back(ost::k_seen, seen.take());
    }
    m_renderer->request_capture();
    save->frames_left = k_capture_frames;
    m_pending_save = std::move(save);
    if (m_ui) {
        m_ui->open("LoadWaitSpinner");
    }
    log::info("save: gathered in {:.2f} ms", ms_between(started, Clock::now()));
}

void App::update_saves(float dt) {
    if (m_save_write.valid() && m_save_write.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        finish_save_write();
    }
    // A load door's autosave comes once the loading screen and the passes after it have gone.
    if (m_loading) {
        m_after_loading = k_after_load_passes;
    } else if (m_after_loading > 0) {
        --m_after_loading;
    } else if (m_autosave_after_move) {
        m_autosave_after_move = false;
        autosave();
    }
    if (m_autosave_in > 0 && --m_autosave_in == 0) {
        // The Tween Menu's: its notice has gone up already.
        if (can_save() && m_character_id) {
            start_game();
            SaveRecord& r = save_record();
            const uint32_t slot = r.autosave_slot;
            const uint32_t count = m_rules ? uint32_t(m_rules->setting("iAutoSaveCount").value_or(3.0)) : 3;
            r.autosave_slot = slot % std::max(count, 1u) + 1;
            const auto old = r.slots.find(slot);
            begin_save(ost::Kind::autosave, save_stem("Autosave", slot), r.next_number,
                       old != r.slots.end() ? old->second : std::filesystem::path());
            r.slots[slot] = m_pending_save->path;
        }
    }
    // Dying: fPlayerDeathReloadTime, then the last save, or the Main Menu without one.
    if (player_dead() && !m_load) {
        m_death_timer += dt; // the update delta, which has the camera's 0.8 in it
        if (m_death_timer > k_death_reload) {
            m_death_timer = 0;
            if (m_last_save.empty()) {
                return_to_main_menu();
            } else {
                m_load_request = m_last_save;
            }
        }
    } else {
        m_death_timer = 0;
    }
    if (!m_pending_save || --m_pending_save->frames_left > 0) {
        return;
    }
    // The picture is sampled small here, so nothing as big as the frame is copied on the main thread; the rest goes on a
    // worker, to a temporary name so a failure leaves the last good file.
    std::unique_ptr<PendingSave> done = std::move(m_pending_save);
    ost::Save& s = done->save;
    uint32_t width = k_thumb_width, height = k_thumb_height;
    if (m_renderer->take_capture_scaled(s.thumbnail, width, height)) {
        s.header.thumb_width = width;
        s.header.thumb_height = height;
    }
    m_last_save = done->path;
    m_last_save_time = Clock::now();
    m_save_write = std::async(std::launch::async, [done = std::move(done)] {
        const std::filesystem::path tmp = done->path.string() + ".tmp";
        std::error_code ec;
        std::filesystem::create_directories(done->path.parent_path(), ec);
        const auto bytes = ost::write(done->save);
        write_file(tmp, bytes);
        std::filesystem::rename(tmp, done->path, ec);
        if (ec) {
            log::warn("save: cannot write {}: {}", done->path.string(), ec.message());
        } else {
            log::info("save: {} ({} KiB)", done->path.filename().string(), bytes.size() / 1024);
            // The old file goes only once the new one is whole.
            if (!done->replaces.empty() && done->replaces != done->path) {
                std::filesystem::remove(done->replaces, ec);
            }
        }
    });
}

void App::finish_save_write() {
    if (m_save_write.valid()) {
        m_save_write.get();
        if (m_ui) {
            m_ui->close("LoadWaitSpinner");
        }
    }
}

const std::vector<ost::Plugin>& App::save_plugins() {
    start_game(); // on a fresh launch nothing has read the load order, and every save would seem to want what's missing
    // The content the change layers sit on. The header names it; loading on another order is allowed, as the game does.
    if (m_save_plugins.empty() && m_game) {
        const forms::Database& db = m_game->db();
        for (size_t i = 0; i < db.plugins().size(); ++i) {
            m_save_plugins.push_back({std::string(db.plugin_name(i)), db.plugins()[i].content_hash});
        }
    }
    return m_save_plugins;
}

std::string App::save_location_text() {
    if (!m_world_sim) {
        return {};
    }
    int32_t cx = 0, cy = 0;
    const WorldSim::Place place = player_place(cx, cy);
    const glm::vec3 at = player_position_units();
    const float position[3] = {at.x, at.y, at.z};
    return m_world_sim->place_name(place, position, cx, cy);
}

std::string App::save_race_text() const {
    if (m_game && m_rules) {
        if (const forms::FormEntry* race = m_game->db().find(m_rules->player.race)) {
            return std::string(forms::Form(m_game->db(), *race).name());
        }
    }
    return {};
}

void App::save_over(int index) {
    // Index 0 is [NEW SAVE]. A Save, Autosave or Quicksave file gives way to a new manual save with the next number.
    const std::filesystem::path old = index > 0 && size_t(index - 1) < m_saves.size() ? m_saves[size_t(index - 1)].path
                                                                                      : std::filesystem::path();
    manual_save(old);
    if (m_ui) {
        m_ui->close("Journal Menu");
    }
}

void App::refresh_saves() {
    finish_save_write();
    m_saves.clear();
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(saves_dir(), ec)) {
        if (path::to_lower(e.path().extension().string()) != ".ost") {
            continue;
        }
        SaveFile file;
        file.path = e.path();
        file.written = e.last_write_time(ec);
        try {
            ost::Save s = ost::read(read_file(e.path()), false);
            file.header = s.header;
            file.location = s.location;
            file.player_name = s.player_name;
            file.race = s.race;
        } catch (const std::exception& error) {
            log::warn("save: {}: {}", e.path().filename().string(), error.what());
            file.corrupt = true;
        }
        if (file.corrupt || m_save_character == k_all_characters || file.header.character == m_save_character) {
            m_saves.push_back(std::move(file));
        }
    }
    // Newest first, by the file's own time.
    std::sort(m_saves.begin(), m_saves.end(), [](const SaveFile& a, const SaveFile& b) { return a.written > b.written; });
}

void App::rescan_records() {
    // On a rescan: the next number after the newest manual save; going from the newest autosave, the slot after it and
    // at most iAutoSaveCount files; the newest quicksave.
    const uint32_t kept = m_save_character;
    m_save_character = k_all_characters;
    refresh_saves();
    m_save_character = kept;
    m_save_records.clear();
    const uint32_t count = m_rules ? uint32_t(m_rules->setting("iAutoSaveCount").value_or(3.0)) : 3;
    std::map<uint64_t, bool> slot_set, number_set;
    for (const SaveFile& s : m_saves) {
        if (s.corrupt) {
            continue;
        }
        const uint64_t key = uint64_t(s.header.character) << 1 | s.header.modded;
        SaveRecord& r = m_save_records[key];
        if (!number_set[key]) {
            number_set[key] = true;
            r.next_number = s.header.number + (s.header.kind == ost::Kind::manual ? 1 : 0);
        }
        if (s.header.kind == ost::Kind::autosave) {
            const std::string stem = s.path.stem().string();
            const uint32_t slot = uint32_t(std::atoi(stem.c_str() + 8)); // past "Autosave"
            if (!slot_set[key]) {
                slot_set[key] = true;
                r.autosave_slot = slot % std::max(count, 1u) + 1;
            }
            if (r.slots.size() < count && !r.slots.contains(slot)) {
                r.slots[slot] = s.path;
            }
        } else if (s.header.kind == ost::Kind::quicksave && r.quicksave.empty()) {
            r.quicksave = s.path;
        }
    }
}

std::vector<ui::SaveListEntry> App::save_list() {
    refresh_saves();
    std::vector<ui::SaveListEntry> out;
    for (const SaveFile& s : m_saves) {
        // fileNum: the abbreviation for an autosave or quicksave, else the number.
        std::string number = std::to_string(s.header.number);
        if (s.header.kind == ost::Kind::autosave) {
            number = m_ui->game_setting("sAutosaveAbbrev");
        } else if (s.header.kind == ost::Kind::quicksave) {
            number = m_ui->game_setting("sQuicksaveAbbrev");
        }
        const std::string place =
            s.location.empty() ? m_ui->game_setting("sMenuDisplayUnknownLocationString") : s.location;
        out.push_back({number, (s.header.modded ? "[M]" : "") + place});
    }
    return out;
}

ui::SaveDetails App::save_details(size_t index) {
    ui::SaveDetails d;
    if (index >= m_saves.size()) {
        return d;
    }
    const SaveFile& s = m_saves[index];
    d.corrupt = s.corrupt;
    d.name = s.player_name;
    d.race = s.race;
    d.level = s.header.level;
    const auto seconds = int64_t(s.header.play_seconds);
    d.play_time = std::format("{:03}.{:02}.{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60);
    const std::time_t when = std::time_t(s.header.saved_at);
    std::tm local{};
    localtime_s(&local, &when);
    d.date = std::format("{}/{}/{:02}", local.tm_mon + 1, local.tm_mday, local.tm_year % 100);
    d.key = s.path.string() + std::to_string(s.header.saved_at);
    try {
        const ost::Save full = ost::read(read_file(s.path), false);
        d.width = full.header.thumb_width;
        d.height = full.header.thumb_height;
        d.image = full.thumbnail;
    } catch (const std::exception&) {
        d.corrupt = true;
    }
    return d;
}

void App::load_save(size_t index) {
    // The asking menu's script is on the stack, and a load closes every menu: the next frame.
    if (index < m_saves.size()) {
        m_load_request = m_saves[index].path;
    }
}

void App::delete_save(size_t index) {
    if (index < m_saves.size()) {
        std::error_code ec;
        std::filesystem::remove(m_saves[index].path, ec);
        m_saves.erase(m_saves.begin() + ptrdiff_t(index));
    }
}

std::vector<ui::SaveCharacter> App::save_characters() {
    // $ShowAllSave, then one entry per character.
    const uint32_t kept = m_save_character;
    m_save_character = k_all_characters;
    refresh_saves();
    m_save_character = kept;
    std::vector<ui::SaveCharacter> out;
    out.push_back({"$ShowAllSave", k_all_characters});
    for (const SaveFile& s : m_saves) {
        const bool known = std::any_of(out.begin(), out.end(), [&](const ui::SaveCharacter& c) { return c.id == s.header.character; });
        if (!known && !s.corrupt) {
            out.push_back({s.player_name, s.header.character});
        }
    }
    return out;
}

void App::select_character(uint32_t id) { m_save_character = id; }

void App::continue_game() {
    // CONTINUE: the newest file of any character.
    m_save_character = k_all_characters;
    refresh_saves();
    if (!m_saves.empty()) {
        m_load_request = m_saves.front().path;
    }
}

// The game. False: asked the player, or gave up.
bool App::save_content_ok(const ost::Save& s, const std::filesystem::path& file) {
    if (std::exchange(m_load_anyway, false) || !m_ui || m_user.number("bDisplayMissingContentDialogue:SaveGame") < 0.5) {
        return true;
    }
    auto answer = [this, file](int button) {
        if (button == 0) {
            m_load_anyway = true;
            m_load_request = file;
        }
    };
    const std::vector<std::string> yes_no = {m_ui->game_setting("sYes"), m_ui->game_setting("sNo")};
    if (!s.header.modded && modded()) {
        m_ui->message(m_ui->game_setting("sLoadVanillaSaveWithMods"), yes_no, answer);
        return false;
    }
    std::string missing;
    const std::vector<ost::Plugin>& here = save_plugins();
    for (const ost::Plugin& p : s.plugins) {
        const std::string name = path::to_lower(p.name);
        if (std::ranges::none_of(here, [&](const ost::Plugin& o) { return path::to_lower(o.name) == name; })) {
            missing += p.name + "\n";
        }
    }
    if (missing.empty()) {
        return true;
    }
    log::info("load: {}: missing {}", file.filename().string(), missing);
    // Asked only where the player chose the save from a list; a quickload in play goes back to the Main Menu.
    if (!m_ui->is_open("Journal Menu") && !m_ui->is_open("Main Menu")) {
        return_to_main_menu();
        return false;
    }
    m_ui->message(std::format("{}\n\n{}\n{}", m_ui->translated("$SaveGameContentNotPresentContinue"), missing,
                              m_ui->translated("$SaveGameObjectsUnavailableContinue")),
                  yes_no, answer);
    return false;
}

bool App::load_game(const std::filesystem::path& file) {
    finish_save_write();
    ost::Save s;
    try {
        s = ost::read(read_file(file));
    } catch (const std::exception& e) {
        log::warn("load: {}: {}", file.string(), e.what());
        if (m_ui) {
            m_ui->message(m_ui->game_setting("sSaveGameCorruptMenuMessage"), {m_ui->game_setting("sOk")});
        }
        return false;
    }
    if (!save_content_ok(s, file)) {
        return false;
    }
    // The screen comes first: the load itself runs once it's up, in finish_load.
    const std::string from_world = m_space ? m_space->edid() : std::string();
    if (m_ui) {
        m_ui->clear_hud_modes();
        m_ui->close_all();
    }
    m_pending_save.reset();
    m_travel_pending = m_travel_fade_marker = 0;
    m_death_timer = 0;
    end_down_view();
    if (m_ui) {
        glm::vec3 feet(0.0f);
        if (const auto* bytes = s.section(ost::k_player)) {
            BinaryReader r(*bytes);
            feet.x = r.read<float>();
            feet.y = r.read<float>();
            feet.z = r.read<float>();
        }
        const float cell = 4096.0f * units::k_skyrim_unit_m;
        m_ui->set_in_world(true);
        m_loading_destination = {};
        m_loading_destination.worldspace = s.space;
        m_loading_destination.cell_x = int32_t(std::floor(feet.x / cell));
        m_loading_destination.cell_y = int32_t(std::floor(feet.y / cell));
        m_loading_destination.from_worldspace = from_world;
        m_loading_destination_set = true;
        m_ui->open("loading");
        m_loading = true;
        m_loading_save_screen = !m_tour && (!m_opt.headless || m_opt.play_clock); // scripts keep their pace
        m_loading_frames = 0;
        m_loading_start = Clock::now();
    }
    m_load = std::make_unique<PendingLoad>(PendingLoad{std::move(s), file});
    if (!m_ui) {
        finish_load();
    }
    return true;
}

// The game's main thread sits in a load while a thread of its own draws the screen (UI_LoadingLoop). Here the main thread
// keeps drawing and the work goes to a worker: only work neither the UI nor the renderer touches. The Loading Menu's conditions read
// no live state (StartingState).
void App::behind_screen(const std::function<void()>& work) {
    if (!m_ui || !m_ui->backdrop()) {
        work();
        return;
    }
    auto job = std::async(std::launch::async, work);
    auto last = Clock::now();
    while (job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        const auto now = Clock::now();
        const float dt = float(ms_between(last, now) / 1000.0);
        last = now;
        SDL_PumpEvents();
        m_ui->update(dt);
        const auto scene = m_ui->backdrop_scene();
        if (!scene) {
            job.wait();
            break;
        }
        m_backdrop->update(*scene, aspect(), dt);
        m_space_views.clear();
        m_space_views.push_back(m_backdrop->space_view());
        render::FrameInput input;
        input.view = &m_backdrop->view();
        input.spaces = m_space_views;
        input.settings = m_render_settings;
        input.time = input.previous_time = m_last_time;
        input.overlay = m_ui->overlay();
        m_ui_scenes->update(m_ui->ui_scenes(), *m_ui);
        input.ui_scenes = m_ui_scenes->inputs();
        m_renderer->render(input);
        m_renderer->take_gpu_timings(m_taken_timings);
    }
    job.get();
}

void App::finish_load() {
    const std::unique_ptr<PendingLoad> pending = std::move(m_load);
    const ost::Save& s = pending->save;
    const std::filesystem::path& file = pending->file;
    const auto load_began = Clock::now();
    m_character_id = s.header.character;
    m_save_character = m_character_id;
    m_play_seconds = s.header.play_seconds;
    m_last_save = file;
    m_autosave_after_move = false;
    m_autosave_in = 0;
    start_game();
    revive_player(); // only here: health is still 0 until the save's comes in, which would kill again
    rescan_records();

    auto reader = [&](container::FourCC id) -> std::optional<BinaryReader> {
        const auto* bytes = s.section(id);
        return bytes ? std::optional<BinaryReader>(BinaryReader(*bytes)) : std::nullopt;
    };
    if (auto r = reader(ost::k_seen); r && m_local_map) {
        m_local_map->seen().load(*r);
    } else if (m_local_map) {
        m_local_map->seen().clear();
    }
    if (auto r = reader(ost::k_clock)) {
        m_clock.load(*r);
        m_sky_hour = m_clock.hour();
    }
    // The space first: the world simulation and the cells it reports need it.
    glm::vec3 feet(0.0f);
    float yaw = 0, pitch = 0;
    bool walk = false;
    if (auto r = reader(ost::k_player)) {
        feet.x = r->read<float>();
        feet.y = r->read<float>();
        feet.z = r->read<float>();
        yaw = r->read<float>();
        pitch = r->read<float>();
        walk = r->read<uint8_t>() != 0;
        const bool custom = r->read<uint8_t>() != 0;
        glm::vec3 marker;
        marker.x = r->read<float>();
        marker.y = r->read<float>();
        marker.z = r->read<float>();
        m_custom_marker = custom ? std::optional<glm::vec3>(marker) : std::nullopt;
        m_last_exterior.x = r->read<float>();
        m_last_exterior.y = r->read<float>();
        m_last_exterior.z = r->read<float>();
        m_last_exterior_world = r->read<uint32_t>();
        m_saving_disabled = r->read<uint8_t>() != 0;
    }
    m_camera.position = feet; // the free camera's position is the player's
    m_camera.yaw = yaw;
    m_camera.pitch = pitch;
    behind_screen([this] { reset_papyrus(); }); // the save's scripts start from nothing
    m_quests_started = false;
    m_loading_save = true;
    load_world(s.space);
    if (walk) {
        set_walk_mode(true);
        m_player->teleport(feet);
        m_player->set_facing(yaw, pitch);
        m_camera.position = m_player->eye();
    }
    m_camera.yaw = yaw;
    m_camera.pitch = pitch;
    if (m_sky) {
        m_sky->set_clock(m_clock);
    }
    m_shadow_direction.valid = false;
    if (auto r = reader(ost::k_world); r && m_world_sim) {
        m_world_sim->load(*r);
    }
    if (const auto* bytes = s.section(ost::k_game); bytes && m_game) {
        m_game->load(*bytes);
    }
    if (const auto* bytes = s.section(ost::k_actors); bytes && m_game) {
        load_actors(*bytes);
    }
    if (m_actor_data) {
        std::unordered_map<uint32_t, NpcGear> all;
        if (auto r = reader(ost::k_npc_gear)) {
            for (auto n = r->read<uint32_t>(); n > 0; --n) {
                NpcGear& g = all[r->read<uint32_t>()];
                const auto flags = r->read<uint8_t>();
                g.bare = (flags & 1) != 0;
                g.headless = (flags & 2) != 0;
                for (std::vector<uint32_t>* list : {&g.on, &g.off}) {
                    list->resize(r->read<uint32_t>());
                    for (uint32_t& form : *list) {
                        form = r->read<uint32_t>();
                    }
                }
            }
        }
        m_actor_data->set_npc_gear(std::move(all));
    }
    if (auto r = reader(ost::k_destruction); r && m_game) {
        if (!m_destruction) {
            m_destruction = std::make_unique<Destruction>(m_game->db());
        }
        m_destruction->load(*r);
        apply_destruction();
    }
    if (auto r = reader(ost::k_sky); r && m_sky) {
        m_sky->force_weather(r->read<uint32_t>());
    }
    if (const auto* bytes = s.section(ost::k_papyrus)) {
        behind_screen([&] {
            ensure_papyrus();
            m_papyrus->vm().load(*bytes);
            if (const auto* quests = s.section(ost::k_quests); quests && m_papyrus->quests()) {
                m_papyrus->quests()->load(*quests);
                attach_dialogue();
                m_quests_started = true; // they carry on from the save: New Game's start isn't run over them
            }
        });
        m_papyrus->player_event("OnPlayerLoadGame");
    }
    if (auto r = reader(ost::k_ui); r && m_ui) {
        std::vector<int> shown(r->read<uint32_t>());
        for (int& v : shown) {
            v = r->read<int>();
        }
        m_ui->set_tutorials_shown(shown);
    }
    m_loading_frames = 0;
    m_load_settle = m_fixed_step > 0.0 && !m_tour && m_papyrus && m_ui ? LoadSettle::wanted : LoadSettle::none;
    log::info("load: {} at {:.0f} {:.0f} {:.0f}, {:.0f} ms", file.filename().string(), feet.x / units::k_skyrim_unit_m,
              feet.y / units::k_skyrim_unit_m, feet.z / units::k_skyrim_unit_m, ms_between(load_began, Clock::now()));
}

} // namespace spud::engine
