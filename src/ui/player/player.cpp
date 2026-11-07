#include "ui/player/player.hpp"

#include "ui/player/draw.hpp"
#include "ui/player/text.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/platform.hpp"

#include <optional>
#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>

namespace spud::ui::player {

namespace {

constexpr double k_deg = 180.0 / std::numbers::pi;
constexpr size_t k_collect_after = 64 * 1024; // allocations between collections

std::string_view str_at(const std::vector<char>& s, uint32_t offset) {
    if (offset >= s.size()) {
        return {};
    }
    return std::string_view(s.data() + offset);
}

bool equal_ignore_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// Movies name each other as the game's file system does ("BethesdaNetLogin.swf", "Interface\\Fonts_en.swf"); the
// cache holds them lower-case under interface/ with the .osu extension.
std::string movie_file_name(std::string_view name) {
    std::string n = path::to_lower(name);
    std::replace(n.begin(), n.end(), '\\', '/');
    while (n.starts_with("./") || n.starts_with("/")) {
        n.erase(0, n[0] == '/' ? 1 : 2);
    }
    if (n.starts_with("interface/")) {
        n.erase(0, 10);
    }
    for (std::string_view ext : {".swf", ".gfx", ".osu"}) {
        if (n.ends_with(ext)) {
            n.resize(n.size() - ext.size());
            break;
        }
    }
    return n + ".osu";
}

Matrix2D from_osu(const osu::Matrix& m) { return {m.a, m.b, m.c, m.d, m.tx, m.ty}; }

ColorXform from_osu(const osu::ColorTransform& c) {
    ColorXform x;
    for (int i = 0; i < 4; ++i) {
        x.mul[i] = c.mul[i];
        x.add[i] = c.add[i];
    }
    return x;
}

// Flash stores positions in twips and colour multipliers in 8.8 fixed point; values read back are those
// (Ruffle: Twips::from_pixels and Fixed8::from_f64 both truncate).
double to_twips(double px) { return std::trunc(px * 20.0) / 20.0; }
float to_fixed8(double v) { return float(std::trunc(std::clamp(v, -128.0, 127.99) * 256.0) / 256.0); }

const char* clip_event_method(uint32_t flag) {
    switch (flag) {
    case ev_load: return "onLoad";
    case ev_enter_frame: return "onEnterFrame";
    case ev_unload: return "onUnload";
    case ev_mouse_move: return "onMouseMove";
    case ev_mouse_down: return "onMouseDown";
    case ev_mouse_up: return "onMouseUp";
    case ev_key_down: return "onKeyDown";
    case ev_key_up: return "onKeyUp";
    case ev_data: return "onData";
    case ev_press: return "onPress";
    case ev_release: return "onRelease";
    case ev_release_outside: return "onReleaseOutside";
    case ev_roll_over: return "onRollOver";
    case ev_roll_out: return "onRollOut";
    case ev_drag_over: return "onDragOver";
    case ev_drag_out: return "onDragOut";
    default: return nullptr;
    }
}

constexpr uint32_t k_button_events =
    ev_press | ev_release | ev_release_outside | ev_roll_over | ev_roll_out | ev_drag_over | ev_drag_out;

// DefineButton2 condition bits for the mouse transitions (BUTTONCONDACTION, read little-endian).
uint32_t button_condition(uint32_t flag) {
    switch (flag) {
    case ev_roll_over: return 1u << 0;      // IdleToOverUp
    case ev_roll_out: return 1u << 1;       // OverUpToIdle
    case ev_press: return 1u << 2;          // OverUpToOverDown
    case ev_release: return 1u << 3;        // OverDownToOverUp
    case ev_drag_out: return 1u << 4;       // OverDownToOutDown
    case ev_drag_over: return 1u << 5;      // OutDownToOverDown
    case ev_release_outside: return 1u << 6; // OutDownToIdle
    default: return 0;
    }
}

bool is_ancestor(const DisplayObject* a, const DisplayObject* d) {
    for (; d; d = d->parent) {
        if (d == a) {
            return true;
        }
    }
    return false;
}

void grow(float b[4], float x0, float y0, float x1, float y1) {
    b[0] = std::min(b[0], x0);
    b[1] = std::min(b[1], y0);
    b[2] = std::max(b[2], x1);
    b[3] = std::max(b[3], y1);
}

} // namespace

// ---- library -------------------------------------------------------------------------------------

Library::Library(std::filesystem::path root, std::string language) : m_root(std::move(root)) {
    // The font configuration the game loads by default (sFontConfigFile:Fonts = Interface\FontConfig.txt).
    try {
        const osu::FontMap map = osu::read_font_map(read_file(m_root / "fontconfig.osu"));
        for (uint32_t lib : map.libraries) {
            if (auto m = movie(str_at(map.strings, lib))) {
                m_font_libraries.push_back(std::move(m));
            }
        }
        for (const auto& n : map.names) {
            m_logical[std::string(str_at(map.strings, n.logical))] = {std::string(str_at(map.strings, n.font)), n.flags};
        }
    } catch (const std::exception& e) {
        errors.push_back(std::format("font map: {}", e.what()));
    }
    try {
        const osu::Strings table = osu::read_strings(read_file(m_root / ("translate_" + language + ".osu")));
        for (const auto& t : table.entries) {
            m_strings.emplace(std::string(str_at(table.strings, t.key)), std::string(str_at(table.strings, t.text)));
        }
    } catch (const std::exception& e) {
        errors.push_back(std::format("translations: {}", e.what()));
    }
    try {
        const osu::Strings table = osu::read_strings(read_file(m_root / "gamesettings.osu"));
        for (const auto& t : table.entries) {
            m_settings.emplace(std::string(str_at(table.strings, t.key)), std::string(str_at(table.strings, t.text)));
        }
    } catch (const std::exception& e) {
        errors.push_back(std::format("game settings: {}", e.what()));
    }
    // The interface textures the importer brought into the cache,
    // indexed once so registering an img:// image never touches the disk.
    {
        std::unordered_set<std::string> textures;
        const std::filesystem::path cache = m_root.parent_path();
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(cache / "textures" / "interface", ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().extension() == ".dds") {
                textures.insert(it->path().lexically_relative(cache).generic_string());
            }
        }
        images.set_cache_textures(std::move(textures));
        if (std::filesystem::is_regular_file(cache / ImageRegistry::k_default_image_path, ec)) {
            images.set_default_image(ImageRegistry::k_default_image_path);
        } else {
            errors.push_back(
                "GFx's default image is not in the cache (skytool ui import): an unregistered img:// "
                "draws nothing");
        }
    }
    try {
        const osu::Strings table = osu::read_strings(read_file(m_root / "globals.osu"));
        for (const auto& t : table.entries) {
            m_globals.emplace(std::string(str_at(table.strings, t.key)),
                              std::strtod(std::string(str_at(table.strings, t.text)).c_str(), nullptr));
        }
    } catch (const std::exception& e) {
        errors.push_back(std::format("globals: {}", e.what()));
    }
}

double Library::global(std::string_view name, double fallback) const {
    auto it = m_globals.find(std::string(name));
    return it == m_globals.end() ? fallback : it->second;
}

const std::string* Library::game_setting(std::string_view name) const {
    auto it = m_settings.find(std::string(name));
    return it == m_settings.end() ? nullptr : &it->second;
}

void Library::read_ahead() {
    // A menu opens in a frame the player is watching: its file is read and parsed before anyone asks, off the disk on the
    // first open as much as after it. The fonts are the libraries' own, loaded already.
    m_reader = std::jthread([this](std::stop_token stop) {
        std::vector<std::filesystem::path> files;
        std::error_code error;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(m_root, error)) {
            if (entry.path().extension() == ".osu" && !entry.path().filename().string().starts_with("fonts_")) {
                files.push_back(entry.path());
            }
        }
        for (const auto& path : files) {
            if (stop.stop_requested()) {
                return;
            }
            try {
                osu::Movie movie = osu::read(read_file(path));
                std::lock_guard lock(m_early_mutex);
                // By the name movie() looks for: the inventory's shared pieces are in a folder of their own.
                m_early.emplace(path::to_lower(path.lexically_relative(m_root).generic_string()), std::move(movie));
            } catch (const std::exception&) {
                // movie() reports it, if anyone ever asks
            }
        }
    });
}

std::shared_ptr<const MovieDef> Library::movie(std::string_view name) {
    const std::string file = movie_file_name(name);
    if (auto it = m_movies.find(file); it != m_movies.end()) {
        return it->second;
    }
    std::shared_ptr<const MovieDef> def;
    try {
        std::optional<osu::Movie> early;
        {
            std::lock_guard lock(m_early_mutex);
            if (const auto it = m_early.find(file); it != m_early.end()) {
                early = std::move(it->second);
                m_early.erase(it);
            }
        }
        def = std::make_shared<MovieDef>(early ? std::move(*early) : osu::read(read_file(m_root / file)), std::string(name));
    } catch (const std::exception& e) {
        errors.push_back(std::format("movie {}: {}", name, e.what()));
    }
    m_movies.emplace(file, def);
    return def;
}

FontRef Library::font(std::string_view name, bool bold, bool italic) {
    const std::string key = std::format("{}|{}{}", name, int(bold), int(italic));
    if (auto it = m_font_cache.find(key); it != m_font_cache.end()) {
        return it->second;
    }
    // The best style match among the faces with that name: exact names first, then names ignoring case.
    FontRef best;
    int best_score = -1;
    for (int pass = 0; pass < 2 && !best; ++pass) {
        for (const auto& lib : m_font_libraries) {
            const auto& fonts = lib->data().fonts;
            for (uint32_t i = 0; i < fonts.size(); ++i) {
                const std::string_view n = lib->string(fonts[i].name);
                if (pass == 0 ? n != name : !equal_ignore_case(n, name)) {
                    continue;
                }
                const bool b = fonts[i].flags & osu::font_bold, it = fonts[i].flags & osu::font_italic;
                const int score = (b == bold ? 2 : 0) + (it == italic ? 1 : 0) + (fonts[i].glyph_count > 0 ? 4 : 0);
                if (score > best_score) {
                    best_score = score;
                    best = {lib.get(), i};
                }
            }
        }
    }
    m_font_cache.emplace(key, best);
    return best;
}

FontRef Library::logical_font(std::string_view name, bool bold, bool italic) {
    auto it = m_logical.find(std::string(name));
    if (it == m_logical.end()) {
        // Font-map names match ignoring case: the Stats Menu upper-cases its requirement line, html tags included
        // (FACE='$EVERYWHEREBOLDFONT'), and the game still draws it in $EverywhereBoldFont (measured: the perk card
        // of).
        it = std::find_if(m_logical.begin(), m_logical.end(),
                          [&](const auto& entry) { return equal_ignore_case(entry.first, name); });
    }
    if (it != m_logical.end()) {
        return font(it->second.font, bold || (it->second.flags & osu::font_bold),
                    italic || (it->second.flags & osu::font_italic));
    }
    return font(name, bold, italic);
}

const std::string* Library::translate(std::string_view key) const {
    auto it = m_strings.find(std::string(key));
    return it == m_strings.end() ? nullptr : &it->second;
}

// ---- player --------------------------------------------------------------------------------------

Player::ScriptScope::~ScriptScope() {
    if (--p.m_script_depth == 0) {
        p.run_queue();
        p.flush_retired();
    }
}

Player::Player(Library& lib, std::shared_ptr<const MovieDef> m, PlayerHost* h)
    : library(lib), host(h), movie(std::move(m)) {
    vm.hooks = this;
    vm.swf_version = int(movie->data().header.source_version);
    vm.fixed_time_ms = 0;
    install_core_library(vm);
    install_display_library(*this);
}

Player::~Player() {
    root.reset();
    m_retired.clear();
}

double Player::frame_rate() const {
    const float r = movie->data().header.frame_rate;
    return r > 0 ? r : 24.0;
}

std::vector<std::string> Player::errors() const {
    std::vector<std::string> out = library.errors;
    out.insert(out.end(), vm.errors.begin(), vm.errors.end());
    return out;
}

void Player::start() {
    ScriptScope scope(*this);
    root = std::make_unique<MovieClip>(*this, movie.get(), osu::k_none, movie->data().header.root_sprite);
    root->object = make_display_as_object(root.get(), m_movieclip_proto);
    m_exec.push_back(root.get());
    run_frame_avm1(root.get());
}

void Player::advance(double dt) {
    const double period = 1000.0 / frame_rate();
    m_time_ms += std::max(0.0, dt) * 1000.0;
    vm.fixed_time_ms = m_time_ms;
    m_frame_accum += std::max(0.0, dt) * 1000.0;
    if (!m_advanced) {
        // GFx's first Advance, Advance(0) included, ends the first frame's actions with the load queue
        // (then): what frame 1 asked to load (the main menu's
        // BethesdaNetLogin.swf) is there before the engine's next calls.
        m_advanced = true;
        land_loads();
    }
    for (int n = 0; n < 2 && m_frame_accum >= period; ++n) {
        m_frame_accum -= period;
        frame();
    }
    if (m_frame_accum >= period) {
        m_frame_accum = std::fmod(m_frame_accum, period); // beyond two catch-up frames, time is dropped
    }
    run_intervals();
    maybe_collect();
}

void Player::step_frame() {
    m_time_ms += 1000.0 / frame_rate();
    vm.fixed_time_ms = m_time_ms;
    frame();
    run_intervals();
    maybe_collect();
}

void Player::frame() {
    if (!root) {
        return;
    }
    {
        ScriptScope scope(*this);
        ++m_frames;
        // Clips run newest first (the AVM1 execution list); clips created during the pass wait for the next frame.
        const std::vector<MovieClip*> list(m_exec.rbegin(), m_exec.rend());
        for (MovieClip* c : list) {
            if (!c->removed) {
                run_frame_avm1(c);
            }
        }
    }
    // GFx processes the load queue after each frame's actions, in the same Advance (then):
    // loads asked for in this frame, or since the last one, land now.
    land_loads();
}

void Player::land_loads() {
    if (m_loads.empty()) {
        return;
    }
    ScriptScope scope(*this);
    std::vector<PendingLoad> loads = std::move(m_loads);
    m_loads.clear();
    for (auto& l : loads) {
        auto* clip = l.clip && l.clip->display && l.clip->display->kind == DisplayKind::clip
                         ? static_cast<MovieClip*>(l.clip->display)
                         : nullptr;
        const bool ok = clip && !clip->removed && load_movie_into(clip, l.url);
        if (l.loaded) {
            l.loaded(ok);
        }
        if (ok && l.initialized) {
            QueuedAction a;
            a.kind = ActionKind::native;
            a.native = std::move(l.initialized);
            push(0, std::move(a));
        }
    }
}

void Player::run_frame_avm1(MovieClip* clip) {
    if (!clip->constructed) {
        fire_clip_event(clip, ev_load);
        clip->current_frame = 0;
        clip->constructed = true;
        clip->enter_frame_commands(0);
        return;
    }
    fire_clip_event(clip, ev_enter_frame);
    if (clip->playing) {
        advance_timeline(clip);
    }
}

void Player::advance_timeline(MovieClip* clip) {
    const uint32_t count = clip->frame_count();
    if (clip->current_frame + 1 < count) {
        ++clip->current_frame;
        clip->enter_frame_commands(clip->current_frame);
    } else if (count > 1) {
        clip->goto_frame(0);
    }
}

void Player::maybe_collect() {
    if (m_script_depth == 0 && vm.heap.allocated_since_collect() > k_collect_after) {
        collect_garbage();
    }
}

// ---- the action queue ----------------------------------------------------------------------------

void Player::push(int priority, QueuedAction a) { m_queue[priority].push_back(std::move(a)); }

void Player::queue_frame_actions(MovieClip* clip, uint32_t frame, uint32_t after_min, uint32_t after_max) {
    if (clip->sprite == osu::k_none) {
        return;
    }
    const osu::Movie& m = clip->movie->data();
    const osu::Sprite& sp = m.sprites[clip->sprite];
    if (frame >= sp.frame_count) {
        return;
    }
    const osu::Frame& fr = m.frames[sp.first_frame + frame];
    for (uint32_t i = 0; i < fr.action_count; ++i) {
        const osu::FrameAction& fa = m.frame_actions[fr.first_action + i];
        if (fa.after < after_min || fa.after > after_max) {
            continue;
        }
        QueuedAction a;
        a.kind = ActionKind::block;
        a.target = clip;
        a.movie = clip->movie;
        a.function = fa.function;
        push(0, std::move(a));
    }
}

void Player::queue_call(DisplayObject* target, std::string_view method, std::vector<Value> args) {
    QueuedAction a;
    a.kind = ActionKind::method;
    a.target = target;
    a.method = std::string(method);
    a.args = std::move(args);
    push(0, std::move(a));
}

void Player::queue_function(Value fn, Value self, std::vector<Value> args) {
    QueuedAction a;
    a.kind = ActionKind::call;
    a.fn = std::move(fn);
    a.self = std::move(self);
    a.args = std::move(args);
    push(0, std::move(a));
}

void Player::broadcast(std::string_view kind, std::string_view method, const std::vector<Value>& args) {
    QueuedAction a;
    a.kind = ActionKind::broadcast;
    a.listener_kind = std::string(kind);
    a.method = std::string(method);
    a.args = args;
    push(0, std::move(a));
}

void Player::run_queue() {
    if (m_draining) {
        return;
    }
    m_draining = true;
    ++m_script_depth;
    for (size_t guard = 0; guard < 4'000'000; ++guard) {
        int pr = 2;
        while (pr >= 0 && m_queue[pr].empty()) {
            --pr;
        }
        if (pr < 0) {
            break;
        }
        QueuedAction a = std::move(m_queue[pr].front());
        m_queue[pr].pop_front();
        execute(a);
    }
    --m_script_depth;
    m_draining = false;
}

void Player::execute(QueuedAction& a) {
    if (a.target && a.target->removed && !a.unload) {
        return;
    }
    switch (a.kind) {
    case ActionKind::block:
        vm.run_block(a.movie, a.function, a.target);
        break;
    case ActionKind::method: {
        Object* o = a.target ? a.target->object : nullptr;
        if (!o) {
            break;
        }
        // GFx dispatches an event method only to an object that has one (the clip's
        // events, or the method on its object or prototypes, found without a display-list search), Load to timeline
        // clips excepted. The dispatch's lookup is a member lookup like any other: on a clip it searches
        // the display list before the prototypes, naming the unnamed children.
        if (!a.look_up && !vm.holder(o, a.method)) {
            break;
        }
        const Value self = Value::object(o);
        const Value fn = vm.get(self, a.method);
        if (fn.is_object() && fn.o->is_function()) {
            vm.call_method(self, a.method, a.args);
        }
        break;
    }
    case ActionKind::call:
        vm.call(a.fn, a.self, a.args);
        break;
    case ActionKind::construct: {
        // The registered class's prototype, then onClipEvent(construct) (component parameters), then the
        // constructor on the existing object.
        Object* o = a.target->object;
        if (!o) {
            break;
        }
        const Value proto = a.fn.is_object() ? vm.get(a.fn, "prototype") : Value{};
        if (proto.is_object()) {
            o->proto = proto.o;
        }
        for (uint32_t f : a.events) {
            vm.run_block(a.movie, f, a.target);
        }
        // GFx's Construct event: after the onClipEvent(construct) handlers, the clip's "onConstruct"
        // method, looked up as any member, so the clip's display list is searched for a child of that name first,
        // which names every unnamed child.
        {
            const Value self = Value::object(o);
            const Value handler = vm.get(self, "onConstruct");
            if (handler.is_object() && handler.o->is_function()) {
                vm.call(handler, self, {});
            }
        }
        if (a.fn.is_object()) {
            vm.define(o, "__constructor__", a.fn);
            vm.define(o, "constructor", a.fn);
            vm.call(a.fn, Value::object(o), {}, proto.object_or_null());
        }
        break;
    }
    case ActionKind::broadcast: {
        const Value broadcaster = vm.global_get(a.listener_kind);
        if (!broadcaster.is_object()) {
            break;
        }
        std::vector<Value> args{Value::string(a.method)};
        args.insert(args.end(), a.args.begin(), a.args.end());
        vm.call_method(broadcaster, "broadcastMessage", args);
        break;
    }
    case ActionKind::native:
        if (a.native) {
            a.native();
        }
        break;
    }
}

// ---- construction --------------------------------------------------------------------------------

Object* Player::make_display_as_object(DisplayObject* obj, Object* proto) {
    const ObjKind kind = obj->kind == DisplayKind::text     ? ObjKind::text_field
                         : obj->kind == DisplayKind::button ? ObjKind::button
                                                            : ObjKind::clip;
    Object* o = vm.heap.make(kind, proto ? proto : vm.object_proto);
    o->display = obj;
    return o;
}

Object* Player::registered_class(const MovieDef* m, uint32_t character) {
    if (!m || character == osu::k_none) {
        return nullptr;
    }
    const std::string_view name = m->export_name_of(character);
    if (name.empty()) {
        return nullptr;
    }
    const Value registry = vm.global_get("__registeredClasses");
    if (!registry.is_object()) {
        return nullptr;
    }
    const Property* p = registry.o->props.find(name);
    return p && p->value.is_object() && p->value.o->is_function() ? p->value.o : nullptr;
}

const MovieDef* Player::find_export(const MovieDef* local, std::string_view id, uint32_t& character) {
    auto own = [&](const MovieDef* m) -> const MovieDef* {
        character = m->export_named(id);
        if (character != osu::k_none) {
            return m;
        }
        // A symbol the movie imports under that name.
        for (const auto& imp : m->data().imports) {
            if (m->string(imp.name) == id) {
                if (auto src = library.movie(m->string(imp.url))) {
                    character = src->export_named(id);
                    if (character != osu::k_none) {
                        return src.get();
                    }
                }
                break;
            }
        }
        return nullptr;
    };
    if (const MovieDef* found = own(local)) {
        return found;
    }
    // GFx's FindExportedResource: the movie defs the root holds are searched next, each one that
    // imports from the last one searched, so a clip of an imported library finds the symbols its
    // importer exports (the Magic Menu's bottom bar buttons attach magicmenu.swf's PS3_LTRT).
    std::vector<const MovieDef*> defs{movie.get()};
    for (const auto& l : m_loaded) {
        defs.push_back(l.get());
    }
    const MovieDef* from = local;
    for (const MovieDef* d : defs) {
        if (d == local) {
            continue;
        }
        const bool imports_from = std::ranges::any_of(d->data().imports, [&](const osu::Import& imp) {
            auto src = library.movie(d->string(imp.url));
            return src && src.get() == from;
        });
        if (imports_from) {
            character = d->export_named(id);
            if (character != osu::k_none) {
                return d;
            }
            from = d;
        }
    }
    character = osu::k_none;
    return nullptr;
}

std::unique_ptr<DisplayObject> Player::make_display_object(const MovieDef* m, uint32_t character) {
    for (int hops = 0; m && hops < 8; ++hops) {
        if (character >= m->data().characters.size()) {
            return nullptr;
        }
        const osu::Character& ch = m->data().characters[character];
        switch (ch.kind) {
        case osu::CharKind::shape: {
            auto s = std::make_unique<ShapeObject>(*this, m, character);
            s->shape = ch.index;
            return s;
        }
        case osu::CharKind::sprite:
            return std::make_unique<MovieClip>(*this, m, character, ch.index);
        case osu::CharKind::text: {
            auto t = std::make_unique<TextFieldObject>(*this, m, character);
            apply_text_definition(t.get());
            return t;
        }
        case osu::CharKind::button: {
            auto b = std::make_unique<ButtonObject>(*this, m, character);
            b->button = ch.index;
            b->set_state(1);
            return b;
        }
        case osu::CharKind::imported: {
            // ImportAssets: the exported character of another movie, which stays loaded with this player.
            const osu::Import& imp = m->data().imports[ch.index];
            auto src = library.movie(m->string(imp.url));
            if (!src) {
                vm.error(std::format("import {} from {}: no such movie", m->string(imp.name), m->string(imp.url)));
                return nullptr;
            }
            character = src->export_named(m->string(imp.name));
            if (character == osu::k_none) {
                vm.error(std::format("import {} from {}: not exported", m->string(imp.name), m->string(imp.url)));
                return nullptr;
            }
            m = src.get();
            if (std::find(m_loaded.begin(), m_loaded.end(), src) == m_loaded.end()) {
                m_loaded.push_back(std::move(src));
            }
            init_library(m);
            continue;
        }
        default:
            return nullptr;
        }
    }
    return nullptr;
}

void Player::apply_place(DisplayObject* obj, const osu::PlaceCommand& c, bool is_new) {
    const MovieDef* placing = obj->parent ? obj->parent->movie : movie.get();
    const osu::Movie& m = placing->data();
    // A script that moved or recoloured an object takes it over from the timeline.
    if ((c.flags & osu::place_matrix) && !obj->script_transformed) {
        obj->set_matrix(from_osu(c.matrix));
    }
    if ((c.flags & osu::place_cxform) && !obj->script_transformed) {
        obj->cxform = from_osu(c.cxform);
    }
    if (c.flags & osu::place_ratio) {
        obj->ratio = c.ratio;
    }
    if (c.flags & osu::place_clip_depth) {
        obj->clip_depth = c.clip_depth;
    }
    if (c.flags & osu::place_filters) {
        obj->filters.assign(m.filters.begin() + c.first_filter, m.filters.begin() + c.first_filter + c.filter_count);
        obj->filter_values.clear();
        for (auto& f : obj->filters) {
            const uint32_t first = uint32_t(obj->filter_values.size());
            obj->filter_values.insert(obj->filter_values.end(), m.filter_values.begin() + f.first_value,
                                      m.filter_values.begin() + f.first_value + f.value_count);
            f.first_value = first;
        }
    }
    if (c.flags & osu::place_blend) {
        obj->blend = c.blend;
    }
    if (c.flags & osu::place_cache_bitmap) {
        obj->cache_as_bitmap = true;
    }
    if (c.flags & osu::place_visible) {
        obj->visible = c.visible != 0;
    }
    if (is_new && (c.flags & osu::place_events)) {
        obj->events_movie = placing;
        obj->first_event = c.first_event;
        obj->event_count = c.event_count;
    }
}

bool Player::trace_names() {
    static const bool on = platform::env("SPUD_UI_TRACE").value_or("") == "names";
    return on;
}

void Player::name_on_creation(DisplayObject* obj) {
    switch (obj->kind) {
    case DisplayKind::shape:
        return; // no script object, no name
    case DisplayKind::text: {
        // A TextField is its own listener (AsBroadcaster): its constructor adds it to its _listeners
        // and the value that holds it asks for its name, named or not. A named one takes a
        // number it then loses to its name.
        const std::string_view saved = name_lookup;
        name_lookup = "text field created";
        if (obj->name_pending) {
            obj->resolved_name();
        } else {
            const std::string lost = default_instance_name(false);
            obj->name_parents();
            if (trace_names()) {
                log::info("ui name: {} for {} (text field created, then named)", lost, obj->target_path());
            }
        }
        name_lookup = saved;
        return;
    }
    default:
        // A named object gets its name handle at once (SetName), and the handle's path names the
        // unnamed parents.
        if (!obj->name_pending) {
            obj->name_parents();
        }
        return;
    }
}

void Player::construct(DisplayObject* obj, const Value& init_object) {
    obj->created_frame = m_frames;
    name_on_creation(obj);
    switch (obj->kind) {
    case DisplayKind::shape:
        return;
    case DisplayKind::text:
        obj->object = make_display_as_object(obj, m_textfield_proto);
        return;
    case DisplayKind::button:
        obj->object = make_display_as_object(obj, m_button_proto);
        return;
    case DisplayKind::clip:
        break;
    }
    auto* clip = static_cast<MovieClip*>(obj);
    clip->object = make_display_as_object(clip, m_movieclip_proto);
    if (Object* init = init_object.object_or_null()) {
        for (const auto& k : vm.enumerate(init)) {
            vm.set(Value::object(clip->object), k, vm.get(init_object, k));
        }
    }
    // Initialize events run before every constructor; the class prototype is set when the constructor runs.
    std::vector<uint32_t> construct_events;
    if (obj->events_movie) {
        const auto& events = obj->events_movie->data().events;
        for (uint32_t i = 0; i < obj->event_count; ++i) {
            const osu::ClipEvent& e = events[obj->first_event + i];
            if (e.events & ev_initialize) {
                QueuedAction a;
                a.kind = ActionKind::block;
                a.target = clip;
                a.movie = obj->events_movie;
                a.function = e.function;
                push(2, std::move(a));
            }
            if (e.events & ev_construct) {
                construct_events.push_back(e.function);
            }
        }
    }
    // Every clip the timeline places gets GFx's Construct event (queued by with the class constructor, or
    // alone), so the action is queued for each.
    Object* cls = registered_class(clip->movie, clip->character);
    {
        QueuedAction a;
        a.kind = ActionKind::construct;
        a.target = clip;
        a.movie = obj->events_movie;
        a.fn = Value::object(cls);
        if (!cls) {
            a.fn = Value{};
        }
        a.events = std::move(construct_events);
        push(1, std::move(a));
    }
    m_exec.push_back(clip);
    run_frame_avm1(clip);
}

void Player::construct_scripted(DisplayObject* obj, const Value& init_object) {
    obj->created_frame = m_frames;
    if (obj->kind != DisplayKind::clip) {
        construct(obj, init_object);
        return;
    }
    auto* clip = static_cast<MovieClip*>(obj);
    name_on_creation(clip);
    Object* cls = registered_class(clip->movie, clip->character);
    const Value proto = cls ? vm.get(Value::object(cls), "prototype") : Value{};
    clip->object = make_display_as_object(clip, proto.is_object() ? proto.o : m_movieclip_proto);
    m_exec.push_back(clip);
    // The first frame (children placed, frame actions queued) before the constructor: constructors read their
    // children (CLIK's UIComponent measures _width).
    run_frame_avm1(clip);
    if (Object* init = init_object.object_or_null()) {
        for (const auto& k : vm.enumerate(init)) {
            vm.set(Value::object(clip->object), k, vm.get(init_object, k));
        }
    }
    if (cls) {
        vm.define(clip->object, "__constructor__", Value::object(cls));
        vm.define(clip->object, "constructor", Value::object(cls));
        vm.call(Value::object(cls), Value::object(clip->object), {}, proto.object_or_null());
    }
}

DisplayObject* Player::replace_character(MovieClip*, DisplayObject* obj, uint32_t character) {
    // Only shapes change character in place (the timeline's shape tweens); other kinds keep theirs.
    if (obj->kind == DisplayKind::shape && character < obj->movie->data().characters.size()) {
        const osu::Character& ch = obj->movie->data().characters[character];
        if (ch.kind == osu::CharKind::shape) {
            obj->character = character;
            obj->placed_character = character;
            static_cast<ShapeObject*>(obj)->shape = ch.index;
        }
    }
    return obj;
}

void Player::run_init_actions(MovieClip* clip, const osu::Frame& frame) {
    // DoInitAction blocks run once per sprite, the first time a frame that holds them is reached, before the
    // frame's other actions and placements (they define the classes the placements need).
    if (frame.init_count == 0) {
        return;
    }
    const osu::Movie& m = clip->movie->data();
    for (uint32_t i = 0; i < frame.init_count; ++i) {
        const osu::InitAction& ia = m.init_actions[frame.first_init + i];
        if (!m_init_run.emplace(clip->movie, ia.sprite_character).second) {
            continue;
        }
        ScriptScope scope(*this);
        vm.run_block(clip->movie, ia.function, clip);
    }
}

void Player::init_library(const MovieDef* m) {
    const osu::Movie& d = m->data();
    if (d.header.root_sprite >= d.sprites.size() || !root) {
        return;
    }
    const osu::Sprite& sp = d.sprites[d.header.root_sprite];
    for (uint32_t f = 0; f < sp.frame_count; ++f) {
        const osu::Frame& fr = d.frames[sp.first_frame + f];
        for (uint32_t i = 0; i < fr.init_count; ++i) {
            const osu::InitAction& ia = d.init_actions[fr.first_init + i];
            if (m_init_run.emplace(m, ia.sprite_character).second) {
                vm.run_block(m, ia.function, root.get());
            }
        }
    }
}

void Player::mark_removed(DisplayObject* obj) {
    if (obj->removed) {
        return;
    }
    obj->removed = true;
    if (obj->kind == DisplayKind::clip) {
        auto* c = static_cast<MovieClip*>(obj);
        for (auto& [d, ch] : c->children) {
            mark_removed(ch.get());
        }
        if (c->mask) {
            c->mask->mask_owner = nullptr;
            c->mask = nullptr;
        }
        if (c->mask_owner) {
            c->mask_owner->mask = nullptr;
            c->mask_owner = nullptr;
        }
        // onUnload runs although the clip is gone (children first).
        fire_clip_event(obj, ev_unload);
    }
}

void Player::on_removed(DisplayObject* obj) {
    mark_removed(obj);
    if (m_hover && is_ancestor(obj, m_hover)) {
        m_hover = nullptr;
    }
    if (m_pressed && is_ancestor(obj, m_pressed)) {
        m_pressed = nullptr;
    }
    if (m_focus && is_ancestor(obj, m_focus)) {
        m_focus = nullptr;
    }
    if (m_drag.clip && is_ancestor(obj, m_drag.clip)) {
        m_drag = {};
    }
}

void Player::retire(std::unique_ptr<DisplayObject> obj) { m_retired.push_back(std::move(obj)); }

void Player::detach(DisplayObject* obj) {
    if (obj->object) {
        if (obj->kind == DisplayKind::clip) {
            obj->object->dead_path = obj->target_path();
        }
        obj->object->display = nullptr;
        obj->object = nullptr;
    }
    if (obj->kind == DisplayKind::clip) {
        for (auto& [d, ch] : static_cast<MovieClip*>(obj)->children) {
            detach(ch.get());
        }
    }
}

void Player::flush_retired() {
    if (m_retired.empty()) {
        return;
    }
    std::erase_if(m_exec, [](MovieClip* c) { return c->removed; });
    for (auto& r : m_retired) {
        detach(r.get());
    }
    m_retired.clear();
}

// ---- clip events ---------------------------------------------------------------------------------

void Player::fire_clip_event(DisplayObject* obj, uint32_t flag, std::vector<Value> args) {
    const bool unload = flag == ev_unload;
    if (obj->events_movie) {
        const auto& events = obj->events_movie->data().events;
        for (uint32_t i = 0; i < obj->event_count; ++i) {
            const osu::ClipEvent& e = events[obj->first_event + i];
            if ((e.events & flag) && flag != ev_initialize && flag != ev_construct) {
                if (flag == ev_key_press && e.key != 0 && e.key != m_last_key && e.key != m_last_ascii) {
                    continue;
                }
                QueuedAction a;
                a.kind = ActionKind::block;
                a.target = obj;
                a.movie = obj->events_movie;
                a.function = e.function;
                a.unload = unload;
                push(0, std::move(a));
            }
        }
    }
    if (const char* name = clip_event_method(flag)) {
        QueuedAction a;
        a.kind = ActionKind::method;
        a.target = obj;
        a.method = name;
        a.args = std::move(args);
        a.unload = unload;
        // GFx dispatches the Load event to every clip the timeline places ( marks them, checks
        // the mark), handler or not.
        a.look_up = flag == ev_load && obj->kind == DisplayKind::clip && obj->placed_by != osu::k_none;
        push(0, std::move(a));
    }
}

void Player::broadcast_clip_event(uint32_t flag, std::string_view, const std::vector<Value>&) {
    // Every clip gets the event, newest first; the method form only where Flash defines it for all clips
    // (onMouseDown/Up/Move). onKeyDown/Up as methods go to the focused clip only.
    const bool method_for_all = flag == ev_mouse_move || flag == ev_mouse_down || flag == ev_mouse_up;
    const std::vector<MovieClip*> list(m_exec.rbegin(), m_exec.rend());
    for (MovieClip* c : list) {
        if (c->removed) {
            continue;
        }
        if (method_for_all) {
            fire_clip_event(c, flag);
            continue;
        }
        if (c->events_movie) {
            const auto& events = c->events_movie->data().events;
            for (uint32_t i = 0; i < c->event_count; ++i) {
                const osu::ClipEvent& e = events[c->first_event + i];
                if (e.events & flag) {
                    QueuedAction a;
                    a.kind = ActionKind::block;
                    a.target = c;
                    a.movie = c->events_movie;
                    a.function = e.function;
                    push(0, std::move(a));
                }
            }
        }
    }
}

// ---- engine -> movie -----------------------------------------------------------------------------

Value Player::resolve_path_value(std::string_view path, Value* holder) {
    std::string p(path);
    Value cur = Value::object(root_object());
    for (std::string_view prefix : {"_root.", "_level0."}) {
        if (p.starts_with(prefix)) {
            p.erase(0, prefix.size());
        }
    }
    if (p.starts_with("_global.")) {
        p.erase(0, 8);
        cur = Value::object(vm.global);
    }
    Value hold = cur;
    size_t start = 0;
    while (start <= p.size()) {
        size_t dot = p.find('.', start);
        if (dot == std::string::npos) {
            dot = p.size();
        }
        hold = cur;
        const std::string_view part = std::string_view(p).substr(start, dot - start);
        // The first name resolves like an ActionScript variable on the root timeline: the root, then _global.
        if (start == 0 && cur.is_object() && cur.o == root_object() && !vm.has(cur.o, part) && vm.has(vm.global, part)) {
            hold = Value::object(vm.global);
            cur = vm.get(hold, part);
        } else {
            cur = vm.get(cur, part);
        }
        start = dot + 1;
        if (dot == p.size()) {
            break;
        }
        if (!cur.is_object()) {
            cur = Value{};
            break;
        }
    }
    if (holder) {
        *holder = hold;
    }
    return cur;
}

Value Player::invoke(std::string_view path, const std::vector<Value>& args) {
    ScriptScope scope(*this);
    Value self;
    const Value fn = resolve_path_value(path, &self);
    if (!fn.is_object() || !fn.o->is_function()) {
        vm.error(std::format("invoke {}: not a function", path));
        return {};
    }
    const size_t dot = path.rfind('.');
    const std::string_view name = dot == std::string_view::npos ? path : path.substr(dot + 1);
    return vm.call_method(self, name, args);
}

bool Player::has_path(std::string_view path) { return !resolve_path_value(path, nullptr).is_undefined(); }

Value Player::get_path(std::string_view path) { return resolve_path_value(path, nullptr); }

void Player::set_path(std::string_view path, const Value& v) {
    ScriptScope scope(*this);
    Value holder;
    resolve_path_value(path, &holder);
    const size_t dot = path.rfind('.');
    vm.set(holder, dot == std::string_view::npos ? path : path.substr(dot + 1), v);
}

Value Player::external_callback(std::string_view name, const std::vector<Value>& args) {
    auto it = m_external_callbacks.find(name);
    if (it == m_external_callbacks.end()) {
        return {};
    }
    ScriptScope scope(*this);
    const auto [self, fn] = it->second;
    return vm.call(fn, self, args);
}

Value Player::invoke_or_callback(std::string_view name, const std::vector<Value>& args) {
    // GFx checks the movie's external callbacks before treating the name as a path.
    if (has_external_callback(name)) {
        return external_callback(name, args);
    }
    return invoke(name, args);
}

// ---- intervals -----------------------------------------------------------------------------------

uint32_t Player::add_interval(Value fn_or_object, Value method, double ms, std::vector<Value> args, bool once) {
    const double period = std::isfinite(ms) ? std::max(ms, 1.0) : 1.0;
    Interval i{m_next_interval++, std::move(fn_or_object), std::move(method), std::move(args), period,
               m_time_ms + period, once};
    m_intervals.push_back(std::move(i));
    return m_intervals.back().id;
}

void Player::clear_interval(uint32_t id) {
    std::erase_if(m_intervals, [&](const Interval& i) { return i.id == id; });
}

void Player::run_intervals() {
    ScriptScope scope(*this);
    std::vector<uint32_t> due;
    for (const auto& i : m_intervals) {
        if (i.next_ms <= m_time_ms) {
            due.push_back(i.id);
        }
    }
    for (uint32_t id : due) {
        auto it = std::find_if(m_intervals.begin(), m_intervals.end(), [&](const Interval& i) { return i.id == id; });
        if (it == m_intervals.end()) {
            continue; // cleared by an earlier callback
        }
        const Interval iv = *it;
        if (iv.once) {
            m_intervals.erase(it);
        } else {
            it->next_ms += iv.period_ms;
            if (it->next_ms <= m_time_ms) {
                it->next_ms = m_time_ms + iv.period_ms;
            }
        }
        if (iv.method.is_undefined()) {
            vm.call(iv.fn_or_object, Value{}, iv.args);
        } else {
            vm.call_method(iv.fn_or_object, *vm.to_string(iv.method), iv.args);
        }
    }
}

// ---- loading -------------------------------------------------------------------------------------

void Player::queue_load(MovieClip* clip, std::string url, std::function<void(bool)> loaded,
                        std::function<void()> initialized) {
    m_loads.push_back({clip->object, std::move(url), std::move(loaded), std::move(initialized)});
}

bool Player::load_movie_into(MovieClip* clip, std::string_view url) {
    // img://NAME is an image the engine supplies through GFx's image loader
    // (render targets such as the map's img://Local_Map,
    // textures such as the books' drop caps), not a movie. GFx makes the clip a one-frame movie of the image
    // at its 64 x 64 (measured: the Map movie's LocalMapHolder_mc reads 1250 % x 703.125 % once its
    // onLoadInit has set it to 800 x 450, run images1). With nothing registered under NAME GFx loads its own
    // 57 x 36 default image instead (the loadMovie of through the game);
    // only without that image's pixels in the cache does the load fail and the clip stay as it was.
    const bool image = !ImageRegistry::name_of(url).empty();
    const uint32_t image_id = image ? library.images.resolve_or_default(url) : ImageRegistry::k_none;
    std::shared_ptr<const MovieDef> m;
    if (image) {
        if (image_id == ImageRegistry::k_none) {
            return false;
        }
    } else if (m = library.movie(url); !m) {
        return false;
    }
    // The clip becomes the loaded movie's main timeline: its children and script properties go, its placement
    // stays (MovieClip.loadMovie).
    std::vector<int> depths;
    for (const auto& [d, c] : clip->children) {
        depths.push_back(d);
    }
    for (int d : depths) {
        clip->remove_child(d);
    }
    clip->drawing.reset();
    clip->engine_image = image_id;
    if (image) {
        clip->character = osu::k_none;
        clip->sprite = osu::k_none;
        clip->current_frame = 0;
        clip->constructed = false;
        clip->playing = false;
        if (clip->object) {
            clip->object->props = PropertyMap();
            clip->object->proto = m_movieclip_proto;
        }
        run_frame_avm1(clip);
        return true;
    }
    clip->movie = m.get();
    clip->character = osu::k_none;
    clip->sprite = m->data().header.root_sprite;
    clip->current_frame = 0;
    clip->constructed = false;
    clip->playing = true;
    if (clip->object) {
        clip->object->props = PropertyMap();
        clip->object->proto = m_movieclip_proto;
    }
    m_loaded.push_back(std::move(m));
    run_frame_avm1(clip);
    return true;
}

// ---- garbage collection --------------------------------------------------------------------------

void Player::collect_garbage() {
    std::vector<Object*> roots{vm.global,          vm.object_proto,   vm.function_proto, vm.array_proto,
                               vm.string_proto,    vm.number_proto,   vm.boolean_proto,  m_movieclip_proto,
                               m_textfield_proto,  m_button_proto};
    auto value = [&](const Value& v) {
        if (v.is_object()) {
            roots.push_back(v.o);
        }
    };
    std::vector<DisplayObject*> stack;
    if (root) {
        stack.push_back(root.get());
    }
    for (auto& r : m_retired) {
        stack.push_back(r.get());
    }
    while (!stack.empty()) {
        DisplayObject* d = stack.back();
        stack.pop_back();
        if (d->object) {
            roots.push_back(d->object);
        }
        if (d->kind == DisplayKind::clip) {
            for (auto& [depth, c] : static_cast<MovieClip*>(d)->children) {
                stack.push_back(c.get());
            }
        }
    }
    for (const auto& q : m_queue) {
        for (const auto& a : q) {
            value(a.fn);
            value(a.self);
            for (const auto& v : a.args) {
                value(v);
            }
        }
    }
    for (const auto& i : m_intervals) {
        value(i.fn_or_object);
        value(i.method);
        for (const auto& v : i.args) {
            value(v);
        }
    }
    for (const auto& [name, cb] : m_external_callbacks) {
        value(cb.first);
        value(cb.second);
    }
    for (const auto& l : m_loads) {
        roots.push_back(l.clip);
    }
    roots.insert(roots.end(), m_pinned.begin(), m_pinned.end());
    std::erase(roots, nullptr);
    vm.heap.collect(roots);
}

// ---- display properties (VmHooks) ----------------------------------------------------------------

namespace {

bool parent_space_bounds(const DisplayObject* d, float& x0, float& y0, float& x1, float& y1) {
    float bx0, by0, bx1, by1;
    if (!d->local_bounds(bx0, by0, bx1, by1)) {
        x0 = y0 = x1 = y1 = 0;
        return false;
    }
    x0 = y0 = std::numeric_limits<float>::max();
    x1 = y1 = std::numeric_limits<float>::lowest();
    const float xs[2] = {bx0, bx1}, ys[2] = {by0, by1};
    for (float x : xs) {
        for (float y : ys) {
            float ox, oy;
            d->matrix.apply(x, y, ox, oy);
            x0 = std::min(x0, ox);
            y0 = std::min(y0, oy);
            x1 = std::max(x1, ox);
            y1 = std::max(y1, oy);
        }
    }
    return true;
}

std::string slash_path(const DisplayObject* d) {
    if (!d->parent) {
        return "/";
    }
    std::string p;
    for (; d && d->parent; d = d->parent) {
        p = "/" + d->resolved_name() + p;
    }
    return p;
}

} // namespace

namespace {

// The built-in display properties are found whatever the case of their name: hudmenu.swf reads a message clip's
// "_currentFrame" (Messages.Update, #322), and the game shows that message for its whole animation (measured: run
// hud-explore1), which it only does when the property resolves. Flash Player keeps these names case-insensitive in
// every SWF version (Ruffle's AVM1 display-property map does the same).
std::string_view display_property_name(std::string_view name, std::string& buffer) {
    if (name.size() < 2 || name[0] != '_') {
        return name;
    }
    bool upper = false;
    for (char c : name) {
        upper |= c >= 'A' && c <= 'Z';
    }
    if (!upper) {
        return name;
    }
    buffer.assign(name);
    for (char& c : buffer) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    static constexpr std::string_view k_builtin[] = {
        "_x",        "_y",         "_xscale",   "_yscale",      "_rotation",     "_alpha",   "_visible",
        "_width",    "_height",    "_name",     "_parent",      "_target",       "_currentframe",
        "_totalframes", "_framesloaded", "_url", "_xmouse",     "_ymouse",       "_droptarget", "_quality",
        "_highquality", "_focusrect", "_soundbuftime"};
    for (std::string_view b : k_builtin) {
        if (buffer == b) {
            return buffer;
        }
    }
    return name;
}

} // namespace

bool Player::get_display(Object* o, std::string_view name, Value& out) {
    DisplayObject* d = o->display;
    if (!d) {
        return false;
    }
    std::string lowered;
    name = display_property_name(name, lowered);
    if (!name.empty() && name[0] == '_') {
        auto* clip = d->kind == DisplayKind::clip ? static_cast<MovieClip*>(d) : nullptr;
        float x0, y0, x1, y1;
        if (name == "_x" || name == "_y") {
            // A text field's _x/_y is the top-left of its box (the definition's bounds start at -2, -2), not its
            // registration point: measured in the game, fields placed at 300.8 and 489.6 report 298.8 and 487.6.
            float px = d->matrix.tx, py = d->matrix.ty;
            if (d->kind == DisplayKind::text) {
                const auto* t = static_cast<const TextFieldObject*>(d);
                d->matrix.apply(t->origin_x, t->origin_y, px, py);
            }
            out = Value::number(name == "_x" ? px : py);
            return true;
        }
        if (name == "_xscale") { out = Value::number(d->scale_x); return true; }
        if (name == "_yscale") { out = Value::number(d->scale_y); return true; }
        if (name == "_rotation") { out = Value::number(d->rotation); return true; }
        if (name == "_alpha") { out = Value::number(double(d->cxform.mul[3]) * 100.0); return true; }
        if (name == "_visible") { out = Value::boolean(d->visible); return true; }
        if (name == "_width") { parent_space_bounds(d, x0, y0, x1, y1); out = Value::number(x1 - x0); return true; }
        if (name == "_height") { parent_space_bounds(d, x0, y0, x1, y1); out = Value::number(y1 - y0); return true; }
        if (name == "_name") { out = Value::string(d->resolved_name()); return true; }
        if (name == "_parent") {
            out = d->parent && d->parent->object ? Value::object(d->parent->object) : Value{};
            return true;
        }
        if (!clip && (name == "_root" || name == "_level0")) { out = Value::object(root_object()); return true; }
        if (!clip && name == "_global") { out = Value::object(vm.global); return true; }
        if (name == "_target") { out = Value::string(slash_path(d)); return true; }
        if (name == "_currentframe") { out = clip ? Value::number(clip->current_frame + 1.0) : Value{}; return true; }
        if (name == "_totalframes" || name == "_framesloaded") {
            out = clip ? Value::number(double(clip->frame_count())) : Value{};
            return true;
        }
        if (name == "_url") { out = Value::string(d->movie ? d->movie->url() : std::string()); return true; }
        if (name == "_xmouse" || name == "_ymouse") {
            float lx, ly;
            d->world_matrix().inverse().apply(m_mouse_x, m_mouse_y, lx, ly);
            out = Value::number(name == "_xmouse" ? lx : ly);
            return true;
        }
        if (name == "_droptarget") { out = Value::string(""); return true; }
        if (name == "_quality") { out = Value::string("HIGH"); return true; }
        if (name == "_highquality") { out = Value::number(1); return true; }
        if (name == "_focusrect") { out = Value::boolean(true); return true; }
        if (name == "_soundbuftime") { out = Value::number(5); return true; }
    }
    if (d->kind == DisplayKind::clip) {
        auto* clip = static_cast<MovieClip*>(d);
        // A sprite's member lookup (GFx): its standard members first, then its own members, then its
        // display list by name (naming the unnamed children it passes), then its prototypes, then _root, _levelN and
        // _global. spud's own members come first and its prototypes after this.
        if (!answered_before_display_list(clip, name)) {
            if (DisplayObject* c = clip->child_named(name); c && c->object) {
                out = Value::object(c->object);
                return true;
            }
        }
        if (name == "_root" || name == "_level0") { out = Value::object(root_object()); return true; }
        if (name == "_global") { out = Value::object(vm.global); return true; }
    }
    return false;
}

bool Player::answered_before_display_list(const MovieClip* clip, std::string_view name) {
    // GFx's standard members (the member table) that a sprite answers itself: the character's getter
    // (blendMode, filters, enabled, trackAsMenu, tabEnabled, tabIndex; useHandCursor only once set, an own
    // member here), the sprite's (_lockroot, focusEnabled, tabChildren, scale9Grid from SWF 8,
    // hitArea once set, hitTestDisable with the GFx extensions on) and GetMember's own cases (transform and the 3D
    // properties). The underscore display properties are answered above. Other standard members a sprite has no
    // getter for (cacheAsBitmap, menu, the text fields' ones) are looked up like any member.
    static constexpr std::string_view k_answered[] = {
        "blendMode", "filters",  "enabled",   "trackAsMenu", "_lockroot", "tabEnabled", "tabIndex",  "focusEnabled",
        "tabChildren", "transform", "_z",     "_zscale",     "_xrotation", "_yrotation", "_matrix3d", "_perspfov"};
    for (std::string_view n : k_answered) {
        if (name == n) {
            return true;
        }
    }
    if (name == "scale9Grid") {
        return clip->movie && clip->movie->data().header.source_version >= 8;
    }
    if (name == "hitArea") {
        return clip->hit_area != nullptr;
    }
    if (name == "hitTestDisable") {
        return vm.to_boolean(vm.global_get("gfxExtensions"));
    }
    return false;
}

bool Player::set_display(Object* o, std::string_view name, const Value& v) {
    DisplayObject* d = o->display;
    if (!d || name.empty() || name[0] != '_') {
        return false;
    }
    std::string lowered;
    name = display_property_name(name, lowered);
    // Numeric display properties ignore values that are not finite numbers.
    auto number = [&](double& out) {
        if (v.is_undefined() || v.is_null()) {
            return false;
        }
        out = vm.to_number(v);
        return std::isfinite(out);
    };
    double n = 0;
    if (name == "_x" || name == "_y") {
        if (number(n)) {
            // Text fields: the box's top-left goes there (see get_display).
            float ox = 0, oy = 0;
            if (d->kind == DisplayKind::text) {
                const auto* t = static_cast<const TextFieldObject*>(d);
                ox = d->matrix.a * t->origin_x + d->matrix.c * t->origin_y;
                oy = d->matrix.b * t->origin_x + d->matrix.d * t->origin_y;
            }
            if (name == "_x") {
                d->matrix.tx = float(to_twips(n - ox));
            } else {
                d->matrix.ty = float(to_twips(n - oy));
            }
            d->script_transformed = true;
        }
        return true;
    }
    if (name == "_xscale" || name == "_yscale") {
        if (number(n)) {
            (name == "_xscale" ? d->scale_x : d->scale_y) = n;
            d->rebuild_matrix();
            d->script_transformed = true;
        }
        return true;
    }
    if (name == "_rotation") {
        if (number(n)) {
            n = std::fmod(n, 360.0);
            if (n > 180) {
                n -= 360;
            } else if (n < -180) {
                n += 360;
            }
            d->rotation = n;
            d->rebuild_matrix();
            d->script_transformed = true;
        }
        return true;
    }
    if (name == "_alpha") {
        if (number(n)) {
            d->cxform.mul[3] = to_fixed8(n / 100.0);
            d->script_transformed = true;
        }
        return true;
    }
    if (name == "_visible") {
        d->visible = vm.to_boolean(v);
        return true;
    }
    if (name == "_width" || name == "_height") {
        if (!number(n)) {
            return true;
        }
        const bool w = name == "_width";
        if (d->kind == DisplayKind::text) {
            auto* t = static_cast<TextFieldObject*>(d);
            (w ? t->width : t->height) = float(std::max(0.0, n));
            t->base_x = t->origin_x;
            t->base_y = t->origin_y;
            t->base_width = t->width;
            t->base_height = t->height;
            text_changed(t);
            return true;
        }
        float x0, y0, x1, y1;
        if (!d->local_bounds(x0, y0, x1, y1)) {
            return true;
        }
        const double ow = double(x1) - x0, oh = double(y1) - y0;
        if ((w && ow <= 0) || (!w && oh <= 0)) {
            return true;
        }
        // Keep the other dimension and solve for this one's scale through the rotation (Ruffle's set_width).
        const double r = d->rotation / k_deg;
        const double cs = std::abs(std::cos(r)), sn = std::abs(std::sin(r));
        if (w) {
            const double aspect = oh / ow, tx = n / ow, ty = d->scale_y / 100.0;
            const double sx = aspect * (cs * tx + sn * ty) / (aspect * cs + sn);
            const double sy = (sn * sx + aspect * cs * ty) / (aspect * cs + sn);
            d->scale_x = sx * 100.0;
            d->scale_y = sy * 100.0;
        } else {
            const double aspect = ow / oh, ty = n / oh, tx = d->scale_x / 100.0;
            const double sy = aspect * (cs * ty + sn * tx) / (aspect * cs + sn);
            const double sx = (sn * sy + aspect * cs * tx) / (aspect * cs + sn);
            d->scale_x = sx * 100.0;
            d->scale_y = sy * 100.0;
        }
        d->rebuild_matrix();
        d->script_transformed = true;
        return true;
    }
    if (name == "_name") {
        d->name = *vm.to_string(v);
        d->name_pending = false;
        return true;
    }
    // Read-only display properties swallow writes.
    for (std::string_view ro : {"_parent", "_target", "_currentframe", "_totalframes", "_framesloaded", "_url",
                                "_xmouse", "_ymouse", "_droptarget"}) {
        if (name == ro) {
            return true;
        }
    }
    return false;
}

Object* Player::root_object() { return root ? root->object : nullptr; }

Object* Player::parent_object(DisplayObject* d) { return d && d->parent ? d->parent->object : nullptr; }

std::string Player::target_path(DisplayObject* d) { return d ? d->target_path() : std::string(); }

Object* Player::display_object(DisplayObject* d) { return d ? d->object : nullptr; }

Object* Player::revive(Object* dead) {
    if (dead->dead_path.empty()) {
        return nullptr;
    }
    DisplayObject* d = resolve_target(root.get(), dead->dead_path);
    return d && d->kind == DisplayKind::clip && d->object ? d->object : nullptr;
}

void Player::goto_frame(DisplayObject* d, uint32_t frame, bool play_after) {
    if (!d || d->kind != DisplayKind::clip) {
        return;
    }
    auto* c = static_cast<MovieClip*>(d);
    c->playing = play_after;
    c->goto_frame(frame);
}

bool Player::goto_label(DisplayObject* d, std::string_view label, bool play_after) {
    if (!d || d->kind != DisplayKind::clip) {
        return false;
    }
    auto* c = static_cast<MovieClip*>(d);
    const int f = c->movie->label_frame(c->sprite, label);
    if (f < 0) {
        return false;
    }
    goto_frame(c, uint32_t(f), play_after);
    return true;
}

void Player::play(DisplayObject* d, bool playing) {
    if (d && d->kind == DisplayKind::clip) {
        static_cast<MovieClip*>(d)->playing = playing;
    }
}

void Player::step(DisplayObject* d, int direction) {
    if (!d || d->kind != DisplayKind::clip) {
        return;
    }
    auto* c = static_cast<MovieClip*>(d);
    if (direction > 0) {
        goto_frame(c, c->current_frame + 1, false);
    } else if (c->current_frame > 0) {
        goto_frame(c, c->current_frame - 1, false);
    } else {
        c->playing = false;
    }
}

DisplayObject* Player::resolve_target(DisplayObject* from, std::string_view path) {
    DisplayObject* cur = from ? from : root.get();
    size_t i = 0;
    if (path.starts_with("/")) {
        cur = root.get();
        i = 1;
    }
    while (i <= path.size() && cur) {
        size_t end = path.find_first_of("/.", i);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        const std::string_view part = path.substr(i, end - i);
        i = end + 1;
        if (part.empty()) {
            if (end == path.size()) {
                break;
            }
            continue;
        }
        if (part == ".." || part == "_parent") {
            cur = cur->parent;
        } else if (part == "_root" || part == "_level0") {
            cur = root.get();
        } else if (part == "this") {
            // stays
        } else {
            DisplayObject* next = cur->kind == DisplayKind::clip ? static_cast<MovieClip*>(cur)->child_named(part)
                                                                 : nullptr;
            if (!next && cur->object) {
                const Value v = vm.get(Value::object(cur->object), part);
                next = v.is_object() ? v.o->display : nullptr;
            }
            cur = next;
        }
        if (end == path.size()) {
            break;
        }
    }
    return cur;
}

// ---- text fields ---------------------------------------------------------------------------------

FontRef Player::find_font(const MovieDef* m, std::string_view name, bool bold, bool italic) {
    if (m) {
        const auto& fonts = m->data().fonts;
        FontRef best;
        int best_score = -1;
        for (uint32_t i = 0; i < fonts.size(); ++i) {
            if (fonts[i].glyph_count == 0 || m->string(fonts[i].name) != name) {
                continue;
            }
            const bool b = fonts[i].flags & osu::font_bold, it = fonts[i].flags & osu::font_italic;
            const int score = (b == bold ? 2 : 0) + (it == italic ? 1 : 0);
            if (score > best_score) {
                best_score = score;
                best = {m, i};
            }
        }
        if (best) {
            return best;
        }
    }
    return library.logical_font(name, bold, italic);
}

std::string Player::translate_text(std::string_view text) const {
    if (!text.empty() && text[0] == '$') {
        if (const std::string* s = library.translate(text)) {
            return *s;
        }
        // A key with a trailing carriage return is tried without it, and its translation replaces the whole text,
        // the break too: the authored <p>$SKILLS</p> (read as "$SKILLS\r") shows "SKILLS" in the game (the Tween
        // Menu, and COST, lvl and per sec in the Favorites Menu: every such field of the game's display lists).
        if (text.size() > 1 && text.back() == '\r') {
            if (const std::string* s = library.translate(text.substr(0, text.size() - 1))) {
                return *s;
            }
        }
    }
    return std::string(text);
}

void Player::apply_text_definition(TextFieldObject* t) {
    const osu::Movie& m = t->movie->data();
    const osu::EditText& e = m.texts[m.characters[t->character].index];
    t->origin_x = t->base_x = e.bounds.x0;
    t->origin_y = t->base_y = e.bounds.y0;
    t->width = t->base_width = e.bounds.x1 - e.bounds.x0;
    t->height = t->base_height = e.bounds.y1 - e.bounds.y0;
    t->word_wrap = e.flags & osu::text_word_wrap;
    t->multiline = e.flags & osu::text_multiline;
    t->password = e.flags & osu::text_password;
    t->editable = !(e.flags & osu::text_read_only);
    t->selectable = !(e.flags & osu::text_no_select);
    t->border = e.flags & osu::text_border;
    t->background = t->border; // DefineEditText's border flag draws both (SWF spec: Border)
    t->html = e.flags & osu::text_html;
    t->embed_fonts = e.flags & osu::text_use_outlines;
    t->auto_size = (e.flags & osu::text_auto_size) ? "left" : "none";
    t->max_chars = e.max_length;
    TextFormat f;
    f.size = e.height;
    if (e.flags & osu::text_has_color) {
        f.color = e.color;
    }
    if (e.font != osu::k_none && e.font < m.characters.size()) {
        const osu::Character& fc = m.characters[e.font];
        if (fc.kind == osu::CharKind::font) {
            const osu::Font& font = m.fonts[fc.index];
            f.font = std::string(m.string(font.name));
            f.bold = font.flags & osu::font_bold;
            f.italic = font.flags & osu::font_italic;
        } else if (fc.kind == osu::CharKind::imported) {
            f.font = std::string(m.string(m.imports[fc.index].name));
        }
    } else if (e.font_class != osu::k_none) {
        f.font = std::string(m.string(e.font_class));
    }
    if (e.flags & osu::text_has_layout) {
        f.align = e.align;
        f.left_margin = e.left_margin;
        f.right_margin = e.right_margin;
        f.indent = e.indent;
        f.leading = e.leading;
    }
    t->default_format = f;
    if (e.variable != osu::k_none) {
        t->variable = std::string(m.string(e.variable));
    }
    t->runs.clear();
    if (e.initial_text != osu::k_none) {
        const std::string_view text = m.string(e.initial_text);
        if (t->html) {
            t->set_html_text(text);
            const std::string plain = u32_to_utf8(t->plain_text());
            const std::string tr = translate_text(plain);
            if (tr != plain) {
                // The translation keeps the format of the text it replaces, as for text set by script: the Tween
                // Menu's "$SKILLS" (<font letterSpacing="6.5">) shows letter-spaced in the game.
                const TextFormat kept = t->runs.empty() ? t->default_format : t->runs.front().format;
                t->set_plain_text(utf8_to_u32(tr));
                t->runs.front().format = kept;
                t->translated_format = kept;
            }
        } else {
            t->set_plain_text(utf8_to_u32(translate_text(text)));
        }
    }
    text_changed(t);
}

void Player::text_changed(TextFieldObject* t) {
    t->layout_dirty = true;
    if (t->auto_size == "none" && t->vertical_auto_size == "none") {
        return;
    }
    // autoSize: the box follows the text (plus the 2-pixel gutter each side), anchored at the left, centre or
    // right; word-wrapped fields keep their width. The height follows too, anchored where Scaleform's
    // verticalAutoSize says (GFx 3 extension), else at the top; verticalAutoSize alone keeps the width. Measured in
    // the game: the Journal's quest description ("top") grows from 35.05 to 60.9.
    //
    // The box is sized from the one authored or last set by script, so anchors set one after another all count:
    // the message box's buttons set autoSize, then verticalAlign and verticalAutoSize "center", then the text,
    // and the game's ButtonText (authored 55.1 high from -14.65) ends 30.05 high at -2.15, centred in the authored
    // box, not at its top. A centred offset drops to a whole twip there (250.5 -> 250 vertically, and Ok's 768.5 ->
    // 768 across: x -11.6), measured in the race menu's question.
    const auto offset = [](float free, std::string_view anchor, std::string_view far) {
        return anchor == far ? free : anchor == "center" ? std::floor(free * 10.0f) / 20.0f : 0.0f;
    };
    t->origin_x = t->base_x;
    t->origin_y = t->base_y;
    t->width = t->base_width;
    t->height = t->base_height;
    const TextLayout& l = t->laid_out();
    const float h = l.text_height + 2 * k_text_gutter;
    if (t->auto_size != "none" && !t->word_wrap) {
        const float w = l.text_width + 2 * k_text_gutter;
        t->origin_x += offset(t->width - w, t->auto_size, "right");
        t->width = w;
    }
    t->origin_y += offset(t->height - h, t->vertical_auto_size, "bottom");
    t->height = h;
    t->layout_dirty = true;
}

// ---- focus and dragging --------------------------------------------------------------------------

void Player::set_focus(DisplayObject* obj) {
    if (obj == m_focus) {
        return;
    }
    DisplayObject* old = m_focus;
    m_focus = obj;
    const Value old_v = old && old->object ? Value::object(old->object) : Value::null();
    const Value new_v = obj && obj->object ? Value::object(obj->object) : Value::null();
    if (old) {
        queue_call(old, "onKillFocus", {new_v});
    }
    if (obj) {
        queue_call(obj, "onSetFocus", {old_v});
    }
    broadcast("Selection", "onSetFocus", {old_v, new_v});
}

void Player::start_drag(MovieClip* clip, bool lock_center, bool constrained, float x0, float y0, float x1, float y1) {
    m_drag = {};
    m_drag.clip = clip;
    m_drag.lock_center = lock_center;
    m_drag.constrained = constrained;
    m_drag.x0 = std::min(x0, x1);
    m_drag.x1 = std::max(x0, x1);
    m_drag.y0 = std::min(y0, y1);
    m_drag.y1 = std::max(y0, y1);
    if (!lock_center) {
        float px = m_mouse_x, py = m_mouse_y;
        if (clip->parent) {
            clip->parent->world_matrix().inverse().apply(m_mouse_x, m_mouse_y, px, py);
        }
        m_drag.off_x = clip->matrix.tx - px;
        m_drag.off_y = clip->matrix.ty - py;
    }
}

void Player::stop_drag() { m_drag = {}; }

// ---- input ---------------------------------------------------------------------------------------

bool Player::wants_mouse(DisplayObject* obj) {
    if (obj->kind == DisplayKind::button) {
        return true;
    }
    if (obj->kind != DisplayKind::clip || !obj->object) {
        return false;
    }
    const Value self = Value::object(obj->object);
    if (!vm.to_boolean(vm.get(self, "enabled"))) {
        return false;
    }
    if (obj->events_movie) {
        const auto& events = obj->events_movie->data().events;
        for (uint32_t i = 0; i < obj->event_count; ++i) {
            if (events[obj->first_event + i].events & k_button_events) {
                return true;
            }
        }
    }
    // The handlers on the clip's object or its prototypes, not its display list: GFx's hit test asks the clip's
    // object (as the game,, does), which names no children.
    for (std::string_view m : {"onPress", "onRelease", "onReleaseOutside", "onRollOver", "onRollOut", "onDragOver",
                               "onDragOut", "onPressAux", "onReleaseAux", "onReleaseOutsideAux", "onDragOverAux",
                               "onDragOutAux"}) {
        Object* h = vm.holder(obj->object, m);
        const Property* p = h ? h->props.find(m) : nullptr;
        const Value f = !p ? Value{} : p->getter ? vm.call(Value::object(p->getter), self, {}, h) : p->value;
        if (f.is_object() && f.o->is_function()) {
            return true;
        }
    }
    return false;
}

DisplayObject* Player::pick(DisplayObject* obj, float x, float y) {
    if (!obj->visible || obj->removed || obj->hit_test_disable) {
        return nullptr;
    }
    switch (obj->kind) {
    case DisplayKind::clip: {
        auto* c = static_cast<MovieClip*>(obj);
        if (c->mask_owner) {
            return nullptr;
        }
        if (c->mask && !c->mask->hit_test_point(x, y, true)) {
            return nullptr;
        }
        if (wants_mouse(c)) {
            const DisplayObject* area = c->hit_area ? c->hit_area : c;
            if (area->hit_test_point(x, y, true)) {
                return c;
            }
        }
        // Children top first; a child under a timeline mask is only reachable inside the mask.
        std::vector<DisplayObject*> masks;
        for (auto it = c->children.begin(); it != c->children.end(); ++it) {
            if (it->second->clip_depth > 0) {
                masks.push_back(it->second.get());
            }
        }
        for (auto it = c->children.rbegin(); it != c->children.rend(); ++it) {
            DisplayObject* ch = it->second.get();
            if (ch->clip_depth > 0) {
                continue;
            }
            const int tag_depth = ch->depth - k_timeline_depth_offset;
            bool outside = false;
            for (DisplayObject* m : masks) {
                const int mask_tag = m->depth - k_timeline_depth_offset;
                if (tag_depth > mask_tag && tag_depth <= m->clip_depth && !m->hit_test_point(x, y, true)) {
                    outside = true;
                    break;
                }
            }
            if (outside) {
                continue;
            }
            if (DisplayObject* r = pick(ch, x, y)) {
                return r;
            }
        }
        return nullptr;
    }
    case DisplayKind::button: {
        auto* b = static_cast<ButtonObject*>(obj);
        const Matrix2D w = b->world_matrix();
        float lx, ly;
        w.inverse().apply(x, y, lx, ly);
        for (const auto& part : b->hit_parts) {
            float px, py;
            part->matrix.inverse().apply(lx, ly, px, py);
            if (part->kind == DisplayKind::shape && static_cast<ShapeObject*>(part.get())->hit_local(px, py)) {
                return b;
            }
            float x0, y0, x1, y1;
            if (part->kind != DisplayKind::shape && part->local_bounds(x0, y0, x1, y1) && px >= x0 && px <= x1 &&
                py >= y0 && py <= y1) {
                return b;
            }
        }
        return nullptr;
    }
    case DisplayKind::text: {
        auto* t = static_cast<TextFieldObject*>(obj);
        return t->editable && t->hit_test_point(x, y, false) ? t : nullptr;
    }
    default:
        return nullptr;
    }
}

DisplayObject* Player::topmost_mouse_target(float x, float y) {
    // topmostLevel objects are above everything, so they are tested first.
    for (auto it = m_topmost.rbegin(); it != m_topmost.rend(); ++it) {
        if (!it->first->removed) {
            if (DisplayObject* r = pick(it->first, x, y)) {
                return r;
            }
        }
    }
    return root ? pick(root.get(), x, y) : nullptr;
}

DisplayObject* Player::topmost_entity(float x, float y, bool mouse_targets_only) {
    if (mouse_targets_only) {
        return topmost_mouse_target(x, y);
    }
    // The innermost object with an ActionScript object whose shapes are under the point, top first.
    std::function<DisplayObject*(DisplayObject*)> find = [&](DisplayObject* d) -> DisplayObject* {
        if (!d->visible || d->removed || d->hit_test_disable) {
            return nullptr;
        }
        if (d->kind == DisplayKind::clip) {
            auto* c = static_cast<MovieClip*>(d);
            if (c->mask && !c->mask->hit_test_point(x, y, true)) {
                return nullptr;
            }
            for (auto it = c->children.rbegin(); it != c->children.rend(); ++it) {
                if (it->second->clip_depth > 0) {
                    continue;
                }
                if (DisplayObject* r = find(it->second.get())) {
                    return r->object ? r : c;
                }
            }
            return nullptr;
        }
        return d->hit_test_point(x, y, true) ? d : nullptr;
    };
    DisplayObject* r = root ? find(root.get()) : nullptr;
    return r && r->object ? r : nullptr;
}

void Player::button_event(DisplayObject* obj, uint32_t flag, std::vector<Value> args) {
    if (!obj || obj->removed) {
        return;
    }
    if (obj->kind == DisplayKind::button) {
        auto* b = static_cast<ButtonObject*>(obj);
        const bool over = flag == ev_roll_over || flag == ev_drag_over || flag == ev_release;
        b->set_state(flag == ev_press || flag == ev_drag_over ? 4 : over ? 2 : 1);
        const osu::Movie& m = b->movie->data();
        const osu::Button& def = m.buttons[b->button];
        const uint32_t cond = button_condition(flag);
        for (uint32_t i = 0; i < def.action_count; ++i) {
            const osu::ButtonAction& a = m.button_actions[def.first_action + i];
            if (a.conditions & cond) {
                QueuedAction q;
                q.kind = ActionKind::block;
                q.target = b->parent; // button actions run on the timeline that holds the button
                q.movie = b->movie;
                q.function = a.function;
                push(0, std::move(q));
            }
        }
        if (const char* name = clip_event_method(flag)) {
            queue_call(b, name, std::move(args));
        }
        return;
    }
    fire_clip_event(obj, flag, std::move(args));
}

void Player::update_hover() {
    DisplayObject* t = topmost_mouse_target(m_mouse_x, m_mouse_y);
    const std::vector<Value> idx{Value::number(0)}; // GFx passes the mouse index
    if (m_mouse_down && m_pressed) {
        if (t != m_hover) {
            if (m_hover == m_pressed) {
                button_event(m_pressed, ev_drag_out, idx);
            } else if (t == m_pressed) {
                button_event(m_pressed, ev_drag_over, idx);
            }
            m_hover = t;
        }
        return;
    }
    if (t != m_hover) {
        DisplayObject* old = m_hover;
        m_hover = t;
        if (old) {
            button_event(old, ev_roll_out, idx);
        }
        if (t) {
            button_event(t, ev_roll_over, idx);
        }
    }
}

void Player::mouse_move(float x, float y) {
    ScriptScope scope(*this);
    m_mouse_x = x;
    m_mouse_y = y;
    if (m_drag.clip && !m_drag.clip->removed) {
        float px = x, py = y;
        if (m_drag.clip->parent) {
            m_drag.clip->parent->world_matrix().inverse().apply(x, y, px, py);
        }
        float nx = px + m_drag.off_x, ny = py + m_drag.off_y;
        if (m_drag.constrained) {
            nx = std::clamp(nx, m_drag.x0, m_drag.x1);
            ny = std::clamp(ny, m_drag.y0, m_drag.y1);
        }
        m_drag.clip->matrix.tx = float(to_twips(nx));
        m_drag.clip->matrix.ty = float(to_twips(ny));
        m_drag.clip->script_transformed = true;
    }
    broadcast_clip_event(ev_mouse_move, "onMouseMove", {});
    broadcast("Mouse", "onMouseMove", {Value::number(0), Value::number(x), Value::number(y)});
    update_hover();
}

void Player::mouse_button(MouseButton b, bool down) {
    ScriptScope scope(*this);
    const double button = b == MouseButton::left ? 1 : b == MouseButton::right ? 2 : 3;
    update_hover();
    const Value target = m_hover && m_hover->object ? Value::object(m_hover->object) : Value{};
    if (b == MouseButton::left) {
        broadcast_clip_event(down ? ev_mouse_down : ev_mouse_up, down ? "onMouseDown" : "onMouseUp", {});
    }
    broadcast("Mouse", down ? "onMouseDown" : "onMouseUp",
              {Value::number(button), target, Value::number(0), Value::number(m_mouse_x), Value::number(m_mouse_y)});
    // GFx's onPress(mouseIndex, keyboardOrMouse), 0 for the mouse. The item menus' onItemSelect acts only when it is
    // not 0: a click there equips through the RightEquip user event, and would otherwise toggle the item right back.
    const std::vector<Value> idx{Value::number(0), Value::number(0)};
    if (b != MouseButton::left) {
        // GFx's auxiliary-button handlers: onPressAux(mouseIndex, button), ...
        if (m_hover && m_hover->object) {
            const std::vector<Value> aux{Value::number(0), Value::number(button - 1)};
            queue_call(m_hover, down ? "onPressAux" : "onReleaseAux", aux);
        }
        return;
    }
    if (down) {
        m_mouse_down = true;
        m_pressed = m_hover;
        if (m_pressed) {
            button_event(m_pressed, ev_press, idx);
            if (m_pressed->kind == DisplayKind::text) {
                set_focus(m_pressed);
            }
        }
        return;
    }
    m_mouse_down = false;
    DisplayObject* pressed = m_pressed;
    m_pressed = nullptr;
    if (pressed) {
        button_event(pressed, pressed == m_hover ? ev_release : ev_release_outside, idx);
        if (pressed != m_hover) {
            m_hover = nullptr; // the object under the mouse rolls over now
        }
    }
    update_hover();
}

void Player::mouse_wheel(float delta) {
    ScriptScope scope(*this);
    update_hover();
    const Value target = m_hover && m_hover->object ? Value::object(m_hover->object) : Value{};
    broadcast("Mouse", "onMouseWheel",
              {Value::number(delta), target, Value::number(0), Value::number(m_mouse_x), Value::number(m_mouse_y)});
}

// A change the user typed into an input field: TextField.onChanged on the field, then on the objects that
// TextField.addListener registered (Flash's field broadcasts onChanged to its _listeners, which start with the field
// itself: ActionScript 2.0 Language Reference, TextField.addListener / TextField.onChanged). CLIK's TextInput keeps
// its text from that listener call (textField.addListener(this), then onChanged copies textField.text).
void Player::user_changed(TextFieldObject* t) {
    if (!t->object) {
        return;
    }
    const Value field = Value::object(t->object);
    queue_call(t, "onChanged", {field});
    const Value listeners = vm.get(field, "_listeners");
    const Value send = vm.get(field, "broadcastMessage");
    if (listeners.is_object() && send.is_object() && send.o->is_function()) {
        queue_function(send, field, {Value::string("onChanged"), field});
    }
}

void Player::key(uint32_t code, uint32_t ascii, bool down) {
    ScriptScope scope(*this);
    if (down) {
        m_keys_down.insert(code);
        m_last_key = code;
        m_last_ascii = ascii;
    } else {
        m_keys_down.erase(code);
    }
    broadcast_clip_event(down ? ev_key_down : ev_key_up, {}, {});
    if (down) {
        broadcast_clip_event(ev_key_press, {}, {});
    }
    if (m_focus && m_focus->kind != DisplayKind::text) {
        queue_call(m_focus, down ? "onKeyDown" : "onKeyUp");
    }
    broadcast("Key", down ? "onKeyDown" : "onKeyUp", {Value::number(0)});
    if (!down || !m_focus || m_focus->kind != DisplayKind::text) {
        return;
    }
    // Editing keys in a focused input field.
    auto* t = static_cast<TextFieldObject*>(m_focus);
    if (!t->editable) {
        return;
    }
    std::u32string text = t->plain_text();
    int caret = std::clamp(t->caret, 0, int(text.size()));
    bool changed = false;
    switch (code) {
    case 8: // Backspace
        if (caret > 0) {
            text.erase(size_t(caret - 1), 1);
            --caret;
            changed = true;
        }
        break;
    case 46: // Delete
        if (caret < int(text.size())) {
            text.erase(size_t(caret), 1);
            changed = true;
        }
        break;
    case 37: caret = std::max(0, caret - 1); break;
    case 39: caret = std::min(int(text.size()), caret + 1); break;
    case 36: caret = 0; break;
    case 35: caret = int(text.size()); break;
    case 13:
        if (t->multiline) {
            text.insert(size_t(caret), 1, U'\r');
            ++caret;
            changed = true;
        }
        break;
    default: break;
    }
    t->caret = t->selection_begin = t->selection_end = caret;
    if (changed) {
        t->set_plain_text(std::move(text));
        text_changed(t);
        user_changed(t);
    }
}

void Player::text_input(std::u32string_view input) {
    ScriptScope scope(*this);
    if (!m_focus || m_focus->kind != DisplayKind::text) {
        return;
    }
    auto* t = static_cast<TextFieldObject*>(m_focus);
    if (!t->editable) {
        return;
    }
    std::u32string text = t->plain_text();
    int caret = std::clamp(t->caret, 0, int(text.size()));
    bool changed = false;
    for (char32_t c : input) {
        if (c < 0x20 || (t->max_chars > 0 && int(text.size()) >= t->max_chars)) {
            continue;
        }
        text.insert(size_t(caret), 1, c);
        ++caret;
        changed = true;
    }
    t->caret = t->selection_begin = t->selection_end = caret;
    if (changed) {
        t->set_plain_text(std::move(text));
        text_changed(t);
        user_changed(t);
    }
}

// ---- drawing -------------------------------------------------------------------------------------

namespace {

DrawItem marker(DrawItem::Kind k) {
    DrawItem i;
    i.kind = k;
    return i;
}

bool needs_layer(osu::Blend b) { return b != osu::Blend::normal; }

// A contour as triangles by ear clipping (simple polygons; Flash closes the contour back to its start).
void triangulate(const std::vector<Drawing::Point>& contour, std::vector<osu::Vertex>& out) {
    std::vector<Drawing::Point> p = contour;
    if (p.size() > 1 && p.front().x == p.back().x && p.front().y == p.back().y) {
        p.pop_back();
    }
    if (p.size() < 3) {
        return;
    }
    double area = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        const auto& a = p[i];
        const auto& b = p[(i + 1) % p.size()];
        area += double(a.x) * b.y - double(b.x) * a.y;
    }
    const float orient = area >= 0 ? 1.0f : -1.0f;
    auto cross = [](Drawing::Point a, Drawing::Point b, Drawing::Point c) {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    std::vector<size_t> idx(p.size());
    for (size_t i = 0; i < idx.size(); ++i) {
        idx[i] = i;
    }
    size_t guard = idx.size() * idx.size();
    while (idx.size() > 3 && guard-- > 0) {
        bool clipped = false;
        for (size_t i = 0; i < idx.size(); ++i) {
            const auto a = p[idx[(i + idx.size() - 1) % idx.size()]], b = p[idx[i]], c = p[idx[(i + 1) % idx.size()]];
            if (cross(a, b, c) * orient <= 0) {
                continue; // reflex or degenerate
            }
            bool inside = false;
            for (size_t j : idx) {
                const auto q = p[j];
                if ((q.x == a.x && q.y == a.y) || (q.x == b.x && q.y == b.y) || (q.x == c.x && q.y == c.y)) {
                    continue;
                }
                if (cross(a, b, q) * orient > 0 && cross(b, c, q) * orient > 0 && cross(c, a, q) * orient > 0) {
                    inside = true;
                    break;
                }
            }
            if (inside) {
                continue;
            }
            out.push_back({a.x, a.y});
            out.push_back({b.x, b.y});
            out.push_back({c.x, c.y});
            idx.erase(idx.begin() + std::ptrdiff_t(i));
            clipped = true;
            break;
        }
        if (!clipped) {
            break; // self-intersecting: what is left is not filled
        }
    }
    if (idx.size() == 3) {
        for (size_t j : idx) {
            out.push_back({p[j].x, p[j].y});
        }
    }
}

// A line from a to b, `half` wide on each side, with round caps: a quad and two half discs. `pixels` is the
// half width on screen, for how many segments a cap needs.
void stroke_segment(float ax, float ay, float bx, float by, float half, float pixels, std::vector<osu::Vertex>& out) {
    float dx = bx - ax, dy = by - ay;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len > 0) {
        dx /= len;
        dy /= len;
        const float nx = -dy * half, ny = dx * half;
        const osu::Vertex q[4] = {{ax + nx, ay + ny}, {bx + nx, by + ny}, {bx - nx, by - ny}, {ax - nx, ay - ny}};
        for (int k : {0, 1, 2, 0, 2, 3}) {
            out.push_back(q[k]);
        }
    } else {
        dx = 1;
        dy = 0;
    }
    const int n = std::clamp(int(std::ceil(pixels * 1.5f)), 2, 16); // segments per half disc
    const float base = std::atan2(dy, dx);
    for (int end = 0; end < 2; ++end) {
        const float cx = end ? bx : ax, cy = end ? by : ay;
        const float start = end ? base - float(std::numbers::pi) * 0.5f : base + float(std::numbers::pi) * 0.5f;
        for (int k = 0; k < n; ++k) {
            const float t0 = start + float(std::numbers::pi) * float(k) / float(n);
            const float t1 = start + float(std::numbers::pi) * float(k + 1) / float(n);
            out.push_back({cx, cy});
            out.push_back({cx + std::cos(t0) * half, cy + std::sin(t0) * half});
            out.push_back({cx + std::cos(t1) * half, cy + std::sin(t1) * half});
        }
    }
}

} // namespace

void Player::render(DrawList& out) {
    out.clear();
    out.stage_width = movie->data().header.stage_width;
    out.stage_height = movie->data().header.stage_height;
    m_topmost.clear();
    if (root) {
        draw(root.get(), Matrix2D{}, ColorXform{}, out, false);
    }
    // GFx topmostLevel objects draw after everything else, in the order they were met.
    m_drawing_topmost = true;
    for (size_t i = 0; i < m_topmost.size(); ++i) {
        const auto [obj, xf] = m_topmost[i];
        draw(obj, xf.first, xf.second, out, false);
    }
    m_drawing_topmost = false;
}

void Player::draw(DisplayObject* obj, const Matrix2D& parent, const ColorXform& parent_cx, DrawList& out, bool mask) {
    if (obj->removed) {
        return;
    }
    auto* clip = obj->kind == DisplayKind::clip ? static_cast<MovieClip*>(obj) : nullptr;
    if (!mask && (!obj->visible || (clip && clip->mask_owner))) {
        return;
    }
    if (!mask && obj->topmost && !m_drawing_topmost) {
        m_topmost.push_back({obj, {parent, parent_cx}});
        return;
    }
    const Matrix2D m = parent * obj->matrix;
    ColorXform cx = mask ? ColorXform{} : parent_cx * obj->cxform;
    if (!mask && cx.mul[3] <= 0 && cx.add[3] <= 0) {
        return; // fully transparent
    }
    MovieClip* script_mask = !mask && clip && clip->mask && !clip->mask->removed ? clip->mask : nullptr;
    auto mask_geometry = [&] {
        const Matrix2D mp = script_mask->parent ? script_mask->parent->world_matrix() : Matrix2D{};
        draw(script_mask, mp, ColorXform{}, out, true);
    };
    if (script_mask) {
        out.items.push_back(marker(DrawItem::Kind::mask_begin));
        mask_geometry();
        out.items.push_back(marker(DrawItem::Kind::mask_content));
    }
    const bool layer = !mask && (!obj->filters.empty() || needs_layer(obj->blend));
    if (layer) {
        // Content renders into the layer without this object's colour transform; the layer composites with it,
        // after its filters.
        DrawItem l = marker(DrawItem::Kind::layer_begin);
        l.blend = obj->blend;
        l.filters = obj->filters.empty() ? nullptr : obj->filters.data();
        l.filter_count = uint32_t(obj->filters.size());
        l.filter_values = obj->filter_values.empty() ? nullptr : obj->filter_values.data();
        l.cxform = cx;
        l.transform = m;
        float x0, y0, x1, y1;
        if (obj->bounds_in(nullptr, x0, y0, x1, y1)) {
            l.bounds[0] = x0;
            l.bounds[1] = y0;
            l.bounds[2] = x1;
            l.bounds[3] = y1;
        }
        out.items.push_back(l);
        cx = ColorXform{};
    }
    if (obj->has_scroll_rect) {
        // scrollRect: the content scrolls by (x, y) and is cut to width x height.
        const float* r = obj->scroll_rect;
        const Matrix2D ms = m * Matrix2D{1, 0, 0, 1, -r[0], -r[1]};
        DrawItem box;
        box.kind = DrawItem::Kind::rect;
        box.transform = ms;
        box.rect[0] = r[0];
        box.rect[1] = r[1];
        box.rect[2] = r[0] + r[2];
        box.rect[3] = r[1] + r[3];
        if (!mask) {
            out.items.push_back(marker(DrawItem::Kind::mask_begin));
            out.items.push_back(box);
            out.items.push_back(marker(DrawItem::Kind::mask_content));
        }
        draw_content(obj, ms, cx, out, mask);
        if (!mask) {
            out.items.push_back(marker(DrawItem::Kind::mask_end));
            out.items.push_back(box);
            out.items.push_back(marker(DrawItem::Kind::mask_done));
        }
    } else {
        draw_content(obj, m, cx, out, mask);
    }
    if (layer) {
        out.items.push_back(marker(DrawItem::Kind::layer_end));
    }
    if (script_mask) {
        out.items.push_back(marker(DrawItem::Kind::mask_end));
        mask_geometry();
        out.items.push_back(marker(DrawItem::Kind::mask_done));
    }
}

void Player::draw_content(DisplayObject* obj, const Matrix2D& m, const ColorXform& cx, DrawList& out, bool mask) {
    switch (obj->kind) {
    case DisplayKind::shape: {
        const auto* s = static_cast<ShapeObject*>(obj);
        const osu::Shape& shape = s->movie->data().shapes[s->shape];
        for (uint32_t i = 0; i < shape.mesh_count; ++i) {
            DrawItem it;
            it.kind = DrawItem::Kind::mesh;
            it.movie = s->movie;
            it.index = shape.first_mesh + i;
            it.transform = m;
            it.cxform = cx;
            out.items.push_back(it);
        }
        break;
    }
    case DisplayKind::clip: {
        auto* c = static_cast<MovieClip*>(obj);
        if (const EngineImage* e = library.images.get(c->engine_image); e && !mask) {
            // The loaded engine image over its box to GFx (64 x 64; the default image 57 x 36,
            //).
            DrawItem it;
            it.kind = DrawItem::Kind::image;
            it.index = c->engine_image;
            it.transform = m;
            it.cxform = cx;
            const float r[4] = {0, 0, e->width, e->height};
            std::copy(r, r + 4, it.rect);
            out.items.push_back(it);
        }
        if (c->drawing) {
            draw_drawing(*c->drawing, m, cx, out);
        }
        if (c->has_scale9 || c->scale9() != osu::k_none) {
            draw_scale9(c, m, cx, out, mask);
        } else {
            draw_children(c, m, cx, out, mask);
        }
        break;
    }
    case DisplayKind::text:
        draw_text(static_cast<TextFieldObject*>(obj), m, cx, out, mask);
        break;
    case DisplayKind::button:
        for (const auto& part : static_cast<ButtonObject*>(obj)->parts) {
            draw(part.get(), m, cx, out, mask);
        }
        break;
    }
}

void Player::draw_drawing(const Drawing& d, const Matrix2D& m, const ColorXform& cx, DrawList& out) {
    // One triangles item per fill and per stroke colour run. Strokes are quads with round caps (the default caps and
    // joints are round, and a cap at each end covers the joint); a hairline is one pixel wide whatever the scale.
    auto emit = [&](uint32_t first, uint32_t color) {
        const uint32_t count = uint32_t(out.vertices.size()) - first;
        if (count == 0) {
            return;
        }
        DrawItem it;
        it.kind = DrawItem::Kind::triangles;
        it.first = first;
        it.count = count;
        it.color = color;
        it.transform = m;
        it.cxform = cx;
        out.items.push_back(it);
    };
    for (const auto& f : d.fills) {
        const uint32_t first = uint32_t(out.vertices.size());
        for (const auto& contour : f.contours) {
            triangulate(contour, out.vertices);
        }
        emit(first, f.color);
    }
    // Pixels per local unit, for hairlines and for how finely to round the caps.
    const float scale = std::sqrt(std::abs(m.a * m.d - m.b * m.c));
    for (size_t i = 0; i < d.strokes.size();) {
        const uint32_t color = d.strokes[i].color;
        const uint32_t first = uint32_t(out.vertices.size());
        for (; i < d.strokes.size() && d.strokes[i].color == color; ++i) {
            const Drawing::Stroke& s = d.strokes[i];
            const float half = (s.width > 0 ? s.width : 1.0f / std::max(scale, 1e-6f)) * 0.5f;
            stroke_segment(s.a.x, s.a.y, s.b.x, s.b.y, half, std::max(scale * half, 0.5f), out.vertices);
        }
        emit(first, color);
    }
}

void Player::draw_children(MovieClip* clip, const Matrix2D& m, const ColorXform& cx, DrawList& out, bool mask) {
    // Timeline masks: a child with a clip depth masks the siblings above it up to that tag depth.
    struct Open {
        int clip_depth;
        DisplayObject* mask;
    };
    std::vector<Open> open;
    auto close = [&] {
        const Open o = open.back();
        open.pop_back();
        out.items.push_back(marker(DrawItem::Kind::mask_end));
        draw(o.mask, m, ColorXform{}, out, true);
        out.items.push_back(marker(DrawItem::Kind::mask_done));
    };
    for (auto& [depth, ch] : clip->children) {
        const int tag_depth = depth - k_timeline_depth_offset;
        while (!open.empty() && tag_depth > open.back().clip_depth) {
            close();
        }
        if (ch->clip_depth > 0) {
            if (!mask) {
                out.items.push_back(marker(DrawItem::Kind::mask_begin));
                draw(ch.get(), m, ColorXform{}, out, true);
                out.items.push_back(marker(DrawItem::Kind::mask_content));
                open.push_back({ch->clip_depth, ch.get()});
            }
            continue;
        }
        draw(ch.get(), m, cx, out, mask);
    }
    while (!open.empty()) {
        close();
    }
}

void Player::draw_scale9(MovieClip* clip, const Matrix2D& m, const ColorXform& cx, DrawList& out, bool mask) {
    // Scale-9 applies to the clip's own shapes; nested clips scale normally (Flash 8).
    const osu::Rect& g = clip->has_scale9 ? clip->scale9_rect : clip->movie->data().rects[clip->scale9()];
    float b[4] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                  std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    float x0, y0, x1, y1;
    if (clip->local_bounds(x0, y0, x1, y1)) {
        grow(b, x0, y0, x1, y1);
    } else {
        b[0] = b[1] = b[2] = b[3] = 0;
    }
    const float sx = std::sqrt(m.a * m.a + m.b * m.b), sy = std::sqrt(m.c * m.c + m.d * m.d);
    for (auto& [depth, ch] : clip->children) {
        if (ch->kind != DisplayKind::shape || ch->clip_depth > 0) {
            draw(ch.get(), m, cx, out, mask);
            continue;
        }
        if (!mask && !ch->visible) {
            continue;
        }
        const auto* s = static_cast<ShapeObject*>(ch.get());
        const osu::Shape& shape = s->movie->data().shapes[s->shape];
        const ColorXform ccx = mask ? ColorXform{} : cx * s->cxform;
        for (uint32_t i = 0; i < shape.mesh_count; ++i) {
            DrawItem it;
            it.kind = DrawItem::Kind::mesh;
            it.movie = s->movie;
            it.index = shape.first_mesh + i;
            it.transform = m;
            it.cxform = ccx;
            it.scale9 = true;
            it.inner = s->matrix;
            std::copy(b, b + 4, it.s9_bounds);
            it.s9_grid[0] = g.x0;
            it.s9_grid[1] = g.y0;
            it.s9_grid[2] = g.x1;
            it.s9_grid[3] = g.y1;
            it.s9_scale[0] = sx;
            it.s9_scale[1] = sy;
            out.items.push_back(it);
        }
    }
}

void Player::draw_text(TextFieldObject* t, const Matrix2D& m, const ColorXform& cx, DrawList& out, bool mask) {
    const TextLayout& l = t->laid_out();
    const float box[4] = {t->origin_x, t->origin_y, t->origin_x + t->width, t->origin_y + t->height};
    auto rect = [&](const float r[4], uint32_t color) {
        DrawItem it;
        it.kind = DrawItem::Kind::rect;
        it.transform = m;
        it.cxform = cx;
        it.color = color;
        std::copy(r, r + 4, it.rect);
        out.items.push_back(it);
    };
    if (!mask && t->background) {
        rect(box, t->background_color);
    }
    if (!mask && t->border) {
        const float w = 1.0f / std::max(1e-3f, std::sqrt(m.a * m.a + m.b * m.b));
        const float h = 1.0f / std::max(1e-3f, std::sqrt(m.c * m.c + m.d * m.d));
        const float top[4] = {box[0], box[1], box[2], box[1] + h}, bottom[4] = {box[0], box[3] - h, box[2], box[3]};
        const float left[4] = {box[0], box[1], box[0] + w, box[3]}, right[4] = {box[2] - w, box[1], box[2], box[3]};
        for (const float* r : {top, bottom, left, right}) {
            rect(r, t->border_color);
        }
    }
    float dy = 0;
    if (t->scroll > 1 && size_t(t->scroll - 1) < l.lines.size()) {
        dy = -(l.lines[size_t(t->scroll - 1)].y - l.lines.front().y);
    }
    const float dx = -float(t->hscroll);
    // Text that leaves the field is clipped to it.
    const bool clip = !mask && (dy != 0 || dx != 0 || l.text_width + 2 * k_text_gutter > t->width + 0.5f ||
                                l.text_height + 2 * k_text_gutter > t->height + 0.5f);
    if (clip) {
        out.items.push_back(marker(DrawItem::Kind::mask_begin));
        rect(box, 0xFFFFFFFFu);
        out.items.push_back(marker(DrawItem::Kind::mask_content));
    }
    // Text taller than the field draws the lines from scroll to bottomScroll, the last line that fits in it entirely:
    // GFx leaves out a line that would cross the field's bottom edge (measured in the game: the Tutorial Menu's
    // help text, 350 x 120 with four lines, shows three).
    float last_baseline = 3.4e38f;
    if (!mask && !l.lines.empty() && l.text_height + 2 * k_text_gutter > t->height + 0.5f) {
        const size_t first = size_t(std::clamp(t->scroll, 1, int(l.lines.size())) - 1);
        const float top = l.lines[first].y - l.lines[first].ascent;
        size_t last = first;
        for (size_t i = first; i < l.lines.size(); ++i) {
            if (l.lines[i].y + l.lines[i].descent - top <= t->height - 2 * k_text_gutter + 0.01f) {
                last = i;
            }
        }
        last_baseline = l.lines[last].y + 0.01f;
    }
    for (const PlacedGlyph& g : l.glyphs) {
        if (g.y > last_baseline) {
            continue;
        }
        DrawItem it;
        it.kind = DrawItem::Kind::glyph;
        it.movie = g.font_movie;
        it.index = g.glyph;
        it.transform = m * Matrix2D{g.size, 0, 0, g.size, t->origin_x + g.x + dx, t->origin_y + g.y + dy};
        it.cxform = cx;
        it.color = g.color;
        out.items.push_back(it);
    }
    if (!mask) {
        for (const PlacedImage& im : l.images) {
            DrawItem it;
            const bool engine = im.engine_image != ImageRegistry::k_none;
            it.kind = engine ? DrawItem::Kind::image : DrawItem::Kind::bitmap;
            it.movie = engine ? nullptr : im.movie;
            it.index = engine ? im.engine_image : im.character;
            it.transform = m;
            it.cxform = cx;
            const float r[4] = {t->origin_x + im.x0 + dx, t->origin_y + im.y0 + dy, t->origin_x + im.x1 + dx,
                                t->origin_y + im.y1 + dy};
            std::copy(r, r + 4, it.rect);
            out.items.push_back(it);
        }
    }
    if (clip) {
        out.items.push_back(marker(DrawItem::Kind::mask_end));
        rect(box, 0xFFFFFFFFu);
        out.items.push_back(marker(DrawItem::Kind::mask_done));
    }
}

} // namespace spud::ui::player
