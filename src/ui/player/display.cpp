#include "ui/player/display.hpp"

#include "core/log.hpp"
#include "ui/player/movie.hpp"
#include "ui/player/player.hpp"
#include "ui/player/text.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace spud::ui::player {

namespace {

constexpr double k_deg = 180.0 / std::numbers::pi;

void grow(float& x0, float& y0, float& x1, float& y1, float x, float y) {
    x0 = std::min(x0, x);
    y0 = std::min(y0, y);
    x1 = std::max(x1, x);
    y1 = std::max(y1, y);
}

void grow_transformed(const Matrix2D& m, float bx0, float by0, float bx1, float by1, float& x0, float& y0, float& x1,
                      float& y1) {
    const float xs[2] = {bx0, bx1}, ys[2] = {by0, by1};
    for (float x : xs) {
        for (float y : ys) {
            float ox, oy;
            m.apply(x, y, ox, oy);
            grow(x0, y0, x1, y1, ox, oy);
        }
    }
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

} // namespace

// ---- maths ---------------------------------------------------------------------------------------

Matrix2D Matrix2D::operator*(const Matrix2D& m) const {
    return {a * m.a + c * m.b, b * m.a + d * m.b, a * m.c + c * m.d, b * m.c + d * m.d, a * m.tx + c * m.ty + tx,
            b * m.tx + d * m.ty + ty};
}

Matrix2D Matrix2D::inverse() const {
    const double det = double(a) * d - double(b) * c;
    if (std::abs(det) < 1e-12) {
        return {1, 0, 0, 1, -tx, -ty};
    }
    const double ia = d / det, ib = -b / det, ic = -c / det, id = a / det;
    return {float(ia), float(ib), float(ic), float(id), float(-(ia * tx + ic * ty)), float(-(ib * tx + id * ty))};
}

ColorXform ColorXform::operator*(const ColorXform& in) const {
    ColorXform o;
    for (int i = 0; i < 4; ++i) {
        o.mul[i] = mul[i] * in.mul[i];
        o.add[i] = add[i] + mul[i] * in.add[i];
    }
    return o;
}

// ---- display objects ------------------------------------------------------------------------------

void DisplayObject::set_matrix(const Matrix2D& m) {
    matrix = m;
    // Decomposition as GFx reports it: scales are column lengths, never negative, rotation the x column's angle,
    // skew the difference to the y column's angle, which carries a mirror flip. Measured: the game's display lists
    // never show a negative _xscale or _yscale (356 dumps, 77139 objects); the Journal's flipped option-stepper
    // arrows read _yscale 100 and _rotation 180 (run core7).
    scale_x = std::sqrt(double(m.a) * m.a + double(m.b) * m.b) * 100.0;
    scale_y = std::sqrt(double(m.c) * m.c + double(m.d) * m.d) * 100.0;
    const double rx = std::atan2(m.b, m.a);
    const double ry = std::atan2(-m.c, m.d);
    rotation = rx * k_deg;
    skew = (ry - rx) * k_deg;
}

void DisplayObject::rebuild_matrix() {
    const double r = rotation / k_deg, sk = (rotation + skew) / k_deg;
    matrix.a = float(scale_x / 100.0 * std::cos(r));
    matrix.b = float(scale_x / 100.0 * std::sin(r));
    matrix.c = float(-scale_y / 100.0 * std::sin(sk));
    matrix.d = float(scale_y / 100.0 * std::cos(sk));
}

Matrix2D DisplayObject::world_matrix() const {
    Matrix2D m = matrix;
    for (const MovieClip* p = parent; p; p = p->parent) {
        m = p->matrix * m;
    }
    return m;
}

bool DisplayObject::bounds_in(const DisplayObject* space, float& x0, float& y0, float& x1, float& y1) const {
    float bx0, by0, bx1, by1;
    if (!local_bounds(bx0, by0, bx1, by1)) {
        return false;
    }
    Matrix2D m = world_matrix();
    if (space) {
        m = space->world_matrix().inverse() * m;
    }
    x0 = y0 = std::numeric_limits<float>::max();
    x1 = y1 = std::numeric_limits<float>::lowest();
    grow_transformed(m, bx0, by0, bx1, by1, x0, y0, x1, y1);
    return true;
}

bool DisplayObject::hit_test_point(float sx, float sy, bool shape_flag) const {
    if (!shape_flag) {
        float x0, y0, x1, y1;
        return bounds_in(nullptr, x0, y0, x1, y1) && sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1;
    }
    float lx, ly;
    world_matrix().inverse().apply(sx, sy, lx, ly);
    switch (kind) {
    case DisplayKind::shape:
        return static_cast<const ShapeObject*>(this)->hit_local(lx, ly);
    case DisplayKind::clip: {
        const auto* mc = static_cast<const MovieClip*>(this);
        for (const auto& [d, c] : mc->children) {
            if (c->visible && c->clip_depth == 0 && c->hit_test_point(sx, sy, true)) {
                return true;
            }
        }
        return false;
    }
    default: {
        float x0, y0, x1, y1;
        return local_bounds(x0, y0, x1, y1) && lx >= x0 && lx <= x1 && ly >= y0 && ly <= y1;
    }
    }
}

const std::string& DisplayObject::resolved_name() const {
    if (name_pending) {
        name_pending = false;
        name = player.default_instance_name(false);
        // The child's number first, then its unnamed parents' (the main menu's Menu_mc.instance23 holds
        // instance22, a text field named at its creation).
        name_parents();
        if (player.trace_names()) {
            log::info("ui name: {} ({})", target_path(),
                      player.name_lookup.empty() ? std::string_view("asked") : player.name_lookup);
        }
    }
    return name;
}

void DisplayObject::name_parents() const {
    // The path is built from the root down ( recurses into the parent before it asks for an object's own
    // name), so the outermost unnamed ancestor is named first: the Console's _root.instance1.instance2.
    if (parent) {
        parent->name_parents();
        parent->resolved_name();
    }
}

std::string DisplayObject::target_path() const {
    if (!parent) {
        return "_level0";
    }
    const std::string parent_path = parent->target_path();
    return parent_path + "." + resolved_name();
}

bool ShapeObject::local_bounds(float& x0, float& y0, float& x1, float& y1) const {
    const auto& s = movie->data().shapes[shape];
    x0 = s.bounds.x0;
    y0 = s.bounds.y0;
    x1 = s.bounds.x1;
    y1 = s.bounds.y1;
    return true;
}

bool ShapeObject::hit_local(float x, float y) const {
    const auto& m = movie->data();
    const auto& s = m.shapes[shape];
    if (x < s.bounds.x0 || x > s.bounds.x1 || y < s.bounds.y0 || y > s.bounds.y1) {
        return false;
    }
    for (uint32_t i = 0; i < s.mesh_count; ++i) {
        const auto& mesh = m.meshes[s.first_mesh + i];
        const osu::Vertex* v = m.vertices.data() + mesh.first_vertex;
        const uint32_t* idx = m.indices.data() + mesh.first_index;
        for (uint32_t t = 0; t + 2 < mesh.index_count; t += 3) {
            const auto& p0 = v[idx[t]];
            const auto& p1 = v[idx[t + 1]];
            const auto& p2 = v[idx[t + 2]];
            const float d1 = (x - p1.x) * (p0.y - p1.y) - (p0.x - p1.x) * (y - p1.y);
            const float d2 = (x - p2.x) * (p1.y - p2.y) - (p1.x - p2.x) * (y - p2.y);
            const float d3 = (x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (y - p0.y);
            const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
            if (!(neg && pos)) {
                return true;
            }
        }
    }
    return false;
}

// ---- movie clips ---------------------------------------------------------------------------------

MovieClip::MovieClip(Player& p, const MovieDef* m, uint32_t ch, uint32_t sp)
    : DisplayObject(DisplayKind::clip, p, m, ch), sprite(sp) {}

MovieClip::~MovieClip() = default;

uint32_t MovieClip::frame_count() const {
    return sprite == osu::k_none ? 1 : std::max<uint32_t>(movie->data().sprites[sprite].frame_count, 1);
}

uint32_t MovieClip::scale9() const {
    return sprite == osu::k_none ? osu::k_none : movie->data().sprites[sprite].scale9;
}

DisplayObject* MovieClip::child_at(int d) const {
    auto it = children.find(d);
    return it == children.end() ? nullptr : it->second.get();
}

DisplayObject* MovieClip::child_named(std::string_view n) const {
    // The lowest depth wins when names repeat (as Flash resolves instance names). GFx searches the display list in
    // depth order and asks each object it passes for its name, which names the unnamed ones up to the
    // match, or all of them when none matches.
    const std::string_view saved = player.name_lookup;
    player.name_lookup = n;
    DisplayObject* found = nullptr;
    for (const auto& [d, c] : children) {
        if (!c->removed && c->resolved_name() == n) {
            found = c.get();
            break;
        }
    }
    player.name_lookup = saved;
    return found;
}

int MovieClip::next_highest_depth() const {
    int d = 0;
    for (const auto& [child_depth, c] : children) {
        d = std::max(d, child_depth + 1);
    }
    return d;
}

namespace {

// A shape (or static text, converted to one): no ActionScript object, so no instance number.
bool is_shape(const MovieDef* m, uint32_t character) {
    const auto& chars = m->data().characters;
    return character < chars.size() && chars[character].kind == osu::CharKind::shape;
}

// The name a timeline object placed without one starts with: shapes get spud's own "shapeN" for its dumps;
// the others wait for their first name request (DisplayObject::resolved_name).
const std::string k_unnamed = "unnamed";
std::string unnamed(Player& p, const MovieDef* m, uint32_t character) {
    return is_shape(m, character) ? p.default_instance_name(true) : k_unnamed;
}

} // namespace

DisplayObject* MovieClip::create_child(const MovieDef* m, uint32_t child_character, int d, std::string n,
                                       uint32_t placed_by_command) {
    remove_child(d);
    std::unique_ptr<DisplayObject> obj = player.make_display_object(m, child_character);
    if (!obj) {
        return nullptr;
    }
    obj->parent = this;
    obj->depth = d;
    obj->name_pending = n == k_unnamed;
    obj->name = obj->name_pending ? std::string() : std::move(n);
    obj->placed_by = placed_by_command;
    obj->placed_character = child_character;
    DisplayObject* raw = obj.get();
    children[d] = std::move(obj);
    return raw;
}

void MovieClip::remove_child(int d) {
    auto it = children.find(d);
    if (it == children.end()) {
        return;
    }
    std::unique_ptr<DisplayObject> obj = std::move(it->second);
    children.erase(it);
    player.on_removed(obj.get());
    player.retire(std::move(obj));
}

void MovieClip::swap_depths(DisplayObject* child, int new_depth) {
    if (!child || child->parent != this || child->depth == new_depth) {
        return;
    }
    auto it = children.find(child->depth);
    std::unique_ptr<DisplayObject> moving = std::move(it->second);
    children.erase(it);
    auto other = children.find(new_depth);
    if (other != children.end()) {
        std::unique_ptr<DisplayObject> displaced = std::move(other->second);
        children.erase(other);
        displaced->depth = moving->depth;
        children[displaced->depth] = std::move(displaced);
    }
    moving->depth = new_depth;
    // A timeline object moved by script no longer belongs to the timeline.
    moving->placed_by = osu::k_none;
    children[new_depth] = std::move(moving);
}

// A goto keeping an object applies its placement whole: no colour transform there means none, not what a later frame
// left (hudmenu.swf's frame 2 tints the stealth meter, which the game shows untinted back on frame 1).
static void clear_unplaced_cxform(DisplayObject* obj, const osu::PlaceCommand& c) {
    if (!(c.flags & osu::place_cxform) && !obj->script_transformed) {
        obj->cxform = {};
    }
}

void MovieClip::enter_frame_commands(uint32_t frame, TakenBack* taken) {
    if (sprite == osu::k_none) {
        return;
    }
    const osu::Movie& m = movie->data();
    const osu::Sprite& sp = m.sprites[sprite];
    if (frame >= sp.frame_count) {
        return;
    }
    const osu::Frame& fr = m.frames[sp.first_frame + frame];
    player.run_init_actions(this, fr);
    for (uint32_t i = 0; i < fr.command_count; ++i) {
        // DoAction tags before this command queue ahead of the load events of what it places (Ruffle processes a
        // frame's tags in stream order; quest_journal.swf's root frame script must run before its pages' onLoad).
        player.queue_frame_actions(this, frame, i, i);
        const uint32_t ci = fr.first_command + i;
        const osu::PlaceCommand& c = m.commands[ci];
        const int d = int(c.depth) + k_timeline_depth_offset;
        if (c.flags & osu::place_remove) {
            remove_child(d);
            continue;
        }
        DisplayObject* obj = child_at(d);
        const bool replace = (c.flags & osu::place_move) && (c.flags & osu::place_character) && obj;
        if (!(c.flags & osu::place_move) || !obj) {
            if (!(c.flags & osu::place_character)) {
                continue;
            }
            if (taken) {
                if (auto it = taken->find(d); it != taken->end() && it->second->placed_by == ci) {
                    // A goto's target frame placing an object it placed before takes that one back.
                    remove_child(d);
                    obj = it->second.get();
                    children[d] = std::move(it->second);
                    taken->erase(it);
                    clear_unplaced_cxform(obj, c);
                    player.apply_place(obj, c, false);
                    continue;
                }
            }
            obj = create_child(movie, c.character, d,
                               (c.flags & osu::place_name) ? std::string(movie->string(c.name))
                                                            : unnamed(player, movie, c.character),
                               ci);
            if (!obj) {
                continue;
            }
            player.apply_place(obj, c, true);
            player.construct(obj, Value{});
        } else {
            if (replace && obj->character != c.character) {
                // PlaceObject2 with move + character: swap the character, keep the placement.
                obj = player.replace_character(this, obj, c.character);
            }
            player.apply_place(obj, c, false);
        }
    }
    player.queue_frame_actions(this, frame, fr.command_count);
}

void MovieClip::goto_frame(uint32_t target) {
    if (sprite == osu::k_none) {
        return;
    }
    const uint32_t count = frame_count();
    target = std::min(target, count - 1);
    if (target == current_frame && constructed) {
        return;
    }
    if (target == current_frame + 1 && constructed) {
        current_frame = target;
        enter_frame_commands(target);
        return;
    }
    // GFx's GotoFrame in two steps: the display list of the frame before the target, from the timeline's
    // placements up to it (applied by), then the target frame's own tags, run as when the
    // timeline reaches it. An object the target frame places again at its depth is taken back (GFx
    // marks the objects placed since,, and a placement of the same character at that depth and frame
    // reuses the marked one). An object of the frame before that the target frame removes or replaces is still made
    // and goes again at once: its text fields take instance numbers (each state of a CLIK button, with a text field
    // per state).
    const osu::Movie& m = movie->data();
    const osu::Sprite& sp = m.sprites[sprite];
    const osu::Frame& tf = m.frames[sp.first_frame + target];
    TakenBack taken;
    for (uint32_t i = 0; i < tf.command_count; ++i) {
        const uint32_t ci = tf.first_command + i;
        const osu::PlaceCommand& c = m.commands[ci];
        const int d = int(c.depth) + k_timeline_depth_offset;
        if ((c.flags & osu::place_remove) || !(c.flags & osu::place_character) || (c.flags & osu::place_move)) {
            continue;
        }
        auto it = children.find(d);
        if (it != children.end() && !it->second->removed && it->second->placed_by == ci &&
            it->second->placed_character == c.character) {
            taken[d] = std::move(it->second);
            children.erase(it);
        }
    }
    std::map<int, TimelineSlot> state;
    if (target > 0) {
        state = timeline_state(target - 1);
    }
    // Timeline objects that are not the frame before's go; script-created ones (depth >= 0) stay.
    std::vector<int> remove;
    for (const auto& [d, c] : children) {
        if (d >= 0) {
            continue;
        }
        auto it = state.find(d);
        // Imported clips (the Magic Menu's item card) keep their instance like any other: a goto that recreated them
        // lost the card's state, which the game keeps.
        if (it == state.end() || c->placed_by != it->second.command ||
            c->placed_character != it->second.character) {
            remove.push_back(d);
        }
    }
    for (int d : remove) {
        remove_child(d);
    }
    for (uint32_t f = constructed && target > current_frame ? current_frame + 1 : 0; f < target; ++f) {
        player.run_init_actions(this, m.frames[sp.first_frame + f]);
    }
    for (auto& [d, s] : state) {
        DisplayObject* obj = child_at(d);
        if (!obj) {
            obj = create_child(movie, s.character, d,
                               (s.props.flags & osu::place_name) ? std::string(movie->string(s.props.name))
                                                                  : unnamed(player, movie, s.character),
                               s.command);
            if (!obj) {
                continue;
            }
            player.apply_place(obj, s.props, true);
            player.construct(obj, Value{});
        } else {
            clear_unplaced_cxform(obj, s.props);
            player.apply_place(obj, s.props, false);
        }
    }
    current_frame = target;
    constructed = true;
    enter_frame_commands(target, &taken);
    // Not placed again after all (a target-frame command always is): removed like any other.
    for (auto& [d, obj] : taken) {
        player.on_removed(obj.get());
        player.retire(std::move(obj));
    }
}

std::map<int, TimelineSlot> MovieClip::timeline_state(uint32_t last) const {
    const osu::Movie& m = movie->data();
    const osu::Sprite& sp = m.sprites[sprite];
    std::map<int, TimelineSlot> state;
    for (uint32_t f = 0; f <= last && f < sp.frame_count; ++f) {
        const osu::Frame& fr = m.frames[sp.first_frame + f];
        for (uint32_t i = 0; i < fr.command_count; ++i) {
            const uint32_t ci = fr.first_command + i;
            const osu::PlaceCommand& c = m.commands[ci];
            const int d = int(c.depth) + k_timeline_depth_offset;
            if (c.flags & osu::place_remove) {
                state.erase(d);
                continue;
            }
            auto it = state.find(d);
            if (!(c.flags & osu::place_move) || it == state.end()) {
                if (!(c.flags & osu::place_character)) {
                    continue;
                }
                state[d] = {ci, c.character, c};
                continue;
            }
            TimelineSlot& s = it->second;
            if ((c.flags & osu::place_character) && c.character != s.character) {
                s.character = c.character;
            }
            const uint32_t keep = s.props.flags;
            osu::PlaceCommand merged = s.props;
            if (c.flags & osu::place_matrix) merged.matrix = c.matrix;
            if (c.flags & osu::place_cxform) merged.cxform = c.cxform;
            if (c.flags & osu::place_ratio) merged.ratio = c.ratio;
            if (c.flags & osu::place_filters) { merged.first_filter = c.first_filter; merged.filter_count = c.filter_count; }
            if (c.flags & osu::place_blend) merged.blend = c.blend;
            if (c.flags & osu::place_visible) merged.visible = c.visible;
            merged.flags = keep | (c.flags & ~(osu::place_move | osu::place_character | osu::place_name));
            s.props = merged;
        }
    }
    return state;
}

bool Drawing::bounds(float& x0, float& y0, float& x1, float& y1) const {
    bool any = false;
    auto add = [&](Point p, float r) {
        x0 = std::min(x0, p.x - r);
        y0 = std::min(y0, p.y - r);
        x1 = std::max(x1, p.x + r);
        y1 = std::max(y1, p.y + r);
        any = true;
    };
    for (const auto& f : fills) {
        for (const auto& c : f.contours) {
            for (Point p : c) {
                add(p, 0);
            }
        }
    }
    for (const auto& s : strokes) {
        add(s.a, s.width * 0.5f);
        add(s.b, s.width * 0.5f);
    }
    return any;
}

bool MovieClip::local_bounds(float& x0, float& y0, float& x1, float& y1) const {
    x0 = y0 = std::numeric_limits<float>::max();
    x1 = y1 = std::numeric_limits<float>::lowest();
    bool any = drawing && drawing->bounds(x0, y0, x1, y1);
    if (const EngineImage* e = player.library.images.get(engine_image)) {
        x0 = std::min(x0, 0.0f);
        y0 = std::min(y0, 0.0f);
        x1 = std::max(x1, e->width);
        y1 = std::max(y1, e->height);
        any = true;
    }
    for (const auto& [d, c] : children) {
        float cx0, cy0, cx1, cy1;
        if (c->removed || !c->local_bounds(cx0, cy0, cx1, cy1)) {
            continue;
        }
        grow_transformed(c->matrix, cx0, cy0, cx1, cy1, x0, y0, x1, y1);
        any = true;
    }
    if (!any) {
        x0 = y0 = x1 = y1 = 0;
    }
    return any;
}

// ---- text fields ---------------------------------------------------------------------------------

std::u32string utf8_to_u32(std::string_view s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = uint8_t(s[i]);
        size_t n = 1;
        if (c >= 0xF0 && i + 3 < s.size()) {
            c = ((c & 7) << 18) | ((uint8_t(s[i + 1]) & 63) << 12) | ((uint8_t(s[i + 2]) & 63) << 6) | (uint8_t(s[i + 3]) & 63);
            n = 4;
        } else if (c >= 0xE0 && i + 2 < s.size()) {
            c = ((c & 15) << 12) | ((uint8_t(s[i + 1]) & 63) << 6) | (uint8_t(s[i + 2]) & 63);
            n = 3;
        } else if (c >= 0xC0 && i + 1 < s.size()) {
            c = ((c & 31) << 6) | (uint8_t(s[i + 1]) & 63);
            n = 2;
        }
        out += char32_t(c);
        i += n;
    }
    return out;
}

std::string u32_to_utf8(std::u32string_view s) {
    std::string out;
    for (char32_t c : s) {
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 63));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 63));
            out += char(0x80 | (c & 63));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 63));
            out += char(0x80 | ((c >> 6) & 63));
            out += char(0x80 | (c & 63));
        }
    }
    return out;
}

TextFieldObject::TextFieldObject(Player& p, const MovieDef* m, uint32_t ch) : DisplayObject(DisplayKind::text, p, m, ch) {}
TextFieldObject::~TextFieldObject() = default;

std::u32string TextFieldObject::plain_text() const {
    std::u32string s;
    for (const auto& r : runs) {
        // An inline image reads as one space in the text (measured: GFx reports "Press   to" for
        // "Press <img ...> to", run hud-explore1).
        s += r.image ? std::u32string(U" ") : r.text;
    }
    return s;
}

void TextFieldObject::set_plain_text(std::u32string text) {
    // New text takes the field's new-text format, not the formatting of what it replaces (Flash 8).
    TextFormat f = translated_format.value_or(default_format);
    runs.clear();
    // Flash keeps \r as the paragraph break in text fields, and a CR LF pair is one break, not two.
    for (size_t at; (at = text.find(U"\r\n")) != std::u32string::npos;) {
        text.erase(at + 1, 1);
    }
    for (auto& c : text) {
        if (c == U'\n') {
            c = U'\r';
        }
    }
    runs.push_back({std::move(text), f});
    layout_dirty = true;
}

const TextLayout& TextFieldObject::laid_out() {
    if (layout_dirty || !layout) {
        layout = layout_text(*this);
        layout_dirty = false;
    }
    return *layout;
}

float TextFieldObject::text_width() { return laid_out().text_width; }
float TextFieldObject::text_height() { return laid_out().text_height; }

bool TextFieldObject::local_bounds(float& x0, float& y0, float& x1, float& y1) const {
    x0 = origin_x;
    y0 = origin_y;
    x1 = origin_x + width;
    y1 = origin_y + height;
    return true;
}

// ---- buttons -------------------------------------------------------------------------------------

ButtonObject::ButtonObject(Player& p, const MovieDef* m, uint32_t ch) : DisplayObject(DisplayKind::button, p, m, ch) {}

void ButtonObject::set_state(int new_state) {
    state = new_state;
    parts = make_parts(new_state);
    if (hit_parts.empty()) {
        hit_parts = make_parts(8);
    }
}

std::vector<std::unique_ptr<DisplayObject>> ButtonObject::make_parts(int state_mask) {
    std::vector<std::unique_ptr<DisplayObject>> out;
    const osu::Movie& m = movie->data();
    const osu::Button& b = m.buttons[button];
    std::vector<const osu::ButtonRecord*> records;
    for (uint32_t i = 0; i < b.record_count; ++i) {
        const auto& r = m.button_records[b.first_record + i];
        if (r.states & state_mask) {
            records.push_back(&r);
        }
    }
    std::sort(records.begin(), records.end(), [](auto* x, auto* y) { return x->depth < y->depth; });
    for (const auto* r : records) {
        auto obj = player.make_display_object(movie, r->character);
        if (!obj) {
            continue;
        }
        obj->parent = nullptr;
        obj->set_matrix(from_osu(r->matrix));
        obj->cxform = from_osu(r->cxform);
        obj->blend = r->blend;
        obj->filters.assign(m.filters.begin() + r->first_filter, m.filters.begin() + r->first_filter + r->filter_count);
        out.push_back(std::move(obj));
    }
    return out;
}

bool ButtonObject::local_bounds(float& x0, float& y0, float& x1, float& y1) const {
    x0 = y0 = std::numeric_limits<float>::max();
    x1 = y1 = std::numeric_limits<float>::lowest();
    bool any = false;
    for (const auto& p : parts) {
        float cx0, cy0, cx1, cy1;
        if (p->local_bounds(cx0, cy0, cx1, cy1)) {
            grow_transformed(p->matrix, cx0, cy0, cx1, cy1, x0, y0, x1, y1);
            any = true;
        }
    }
    return any;
}

} // namespace spud::ui::player
