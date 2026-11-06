#pragma once

// The display list: movie clips (sprite instances with timelines), shapes, text fields and buttons, as the
// Flash Player 8 / Scaleform GFx 3 model defines them.
//
// Depths are ActionScript depths: timeline objects sit at (tag depth - 16384), so they are negative;
// script-created ones (attachMovie, createEmptyMovieClip, ...) at >= 0.

#include "ui/player/object.hpp"

#include "formats/osu.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace spud::ui::player {

class MovieDef;
class MovieClip;
class Player;
struct TextLayout;

constexpr int k_timeline_depth_offset = -16384;

enum class DisplayKind : uint8_t { clip, shape, text, button };

struct Matrix2D {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    Matrix2D operator*(const Matrix2D& m) const; // this applied after m
    void apply(float x, float y, float& ox, float& oy) const {
        ox = a * x + c * y + tx;
        oy = b * x + d * y + ty;
    }
    Matrix2D inverse() const;
};

struct ColorXform {
    float mul[4] = {1, 1, 1, 1};
    float add[4] = {0, 0, 0, 0}; // 0..1
    ColorXform operator*(const ColorXform& inner) const; // this applied after inner
};

class DisplayObject {
public:
    DisplayObject(DisplayKind kind, Player& player, const MovieDef* movie, uint32_t character)
        : kind(kind), player(player), movie(movie), character(character) {}
    virtual ~DisplayObject() = default;

    DisplayKind kind;
    Player& player;
    const MovieDef* movie;
    uint32_t character; // index into the movie's characters (osu::k_none for empty clips)
    MovieClip* parent = nullptr;
    int depth = 0;
    // An object placed without a name gets "instanceN" only when its name is first asked for (resolved_name):
    // GFx names lazily, from a per-movie count ( calling, "instance{0}", the count at the movie
    // root's), so the numbers follow the order names are requested. Who asks, and when, is in
    // (instance names): text fields at their creation, a clip's display-list search by
    // name, the Construct event, a named or newly named child's path.
    mutable std::string name;
    mutable bool name_pending = false;
    const std::string& resolved_name() const;
    // The unnamed parents named, outermost first: GFx's name handle holds the object's full path, built from the
    // root down ( through), whenever an object is named or given a name.
    void name_parents() const;
    Object* object = nullptr; // the ActionScript object (clips, text fields, buttons)

    // Placement
    Matrix2D matrix;
    ColorXform cxform;
    bool visible = true;
    osu::Blend blend = osu::Blend::normal;
    std::vector<osu::Filter> filters;
    std::vector<float> filter_values;
    int clip_depth = 0; // > 0: masks the siblings at tag depths (its tag depth, clip_depth]
    uint16_t ratio = 0;
    bool cache_as_bitmap = false;
    uint32_t placed_by = osu::k_none; // the place command that created it (timeline identity)
    // The character it was placed as, in the placing movie's table: an imported clip's own `character` is the
    // exporting movie's id, so the timeline compares this one.
    uint32_t placed_character = osu::k_none;
    bool script_transformed = false;  // a script moved it: the timeline no longer does
    bool removed = false;
    uint64_t created_frame = 0;       // the player frame it was created on (not advanced on that frame)
    // Clip events (onClipEvent / on) the timeline attached, in the placing movie's events table.
    const MovieDef* events_movie = nullptr;
    uint32_t first_event = 0, event_count = 0;

    // Set by script: Flash 8 display options and GFx extensions.
    bool topmost = false;          // GFx topmostLevel: drawn above everything else
    bool hit_test_disable = false; // GFx hitTestDisable: the mouse passes through
    bool has_scroll_rect = false;  // scrollRect: x, y, width, height in own space
    float scroll_rect[4] = {0, 0, 0, 0};
    bool has_scale9 = false; // scale9Grid set by script (overrides the sprite's DefineScalingGrid)
    osu::Rect scale9_rect{};

    // The values ActionScript reads back, kept so that setting one doesn't disturb the others.
    double scale_x = 100, scale_y = 100, rotation = 0, skew = 0; // percent, degrees
    void set_matrix(const Matrix2D& m);          // from the timeline: updates the decomposition
    void rebuild_matrix();                        // from scale/rotation after a script change
    float alpha() const { return cxform.mul[3]; }

    Matrix2D world_matrix() const;
    // Bounds in this object's own space, and in another object's space.
    virtual bool local_bounds(float& x0, float& y0, float& x1, float& y1) const = 0;
    bool bounds_in(const DisplayObject* space, float& x0, float& y0, float& x1, float& y1) const;
    bool hit_test_point(float stage_x, float stage_y, bool shape_flag) const;
    std::string target_path() const;
};

class ShapeObject final : public DisplayObject {
public:
    ShapeObject(Player& player, const MovieDef* movie, uint32_t character)
        : DisplayObject(DisplayKind::shape, player, movie, character) {}
    uint32_t shape = 0; // index into the movie's shapes
    bool local_bounds(float& x0, float& y0, float& x1, float& y1) const override;
    bool hit_local(float x, float y) const;
};

// A frame's timeline state at one depth (for gotos).
// What a clip's drawing methods made (MovieClip.lineStyle, moveTo, lineTo, curveTo, beginFill, endFill, clear;
// ActionScript 2.0 Language Reference, Flash 8). Clip space; drawn below the clip's children.
struct Drawing {
    struct Point {
        float x, y;
    };
    struct Stroke {
        Point a, b;
        float width;    // 0: hairline (one pixel on screen)
        uint32_t color; // RGBA8, straight alpha
    };
    struct Fill {
        std::vector<std::vector<Point>> contours;
        uint32_t color;
    };
    std::vector<Fill> fills;     // in drawing order, below the strokes
    std::vector<Stroke> strokes;
    // The pen.
    Point pen{0, 0};
    bool has_line = false;
    float line_width = 0;
    uint32_t line_color = 0xFF000000u;
    bool filling = false;
    Fill fill;           // the open fill
    Point contour_start{0, 0};
    bool bounds(float& x0, float& y0, float& x1, float& y1) const;
};

struct TimelineSlot {
    uint32_t command = osu::k_none; // the PlaceCommand that created the object
    uint32_t character = osu::k_none;
    osu::PlaceCommand props{};      // merged properties as of the frame
};

class MovieClip final : public DisplayObject {
public:
    MovieClip(Player& player, const MovieDef* movie, uint32_t character, uint32_t sprite);
    ~MovieClip() override;

    uint32_t sprite; // index into the movie's sprites
    uint32_t current_frame = 0; // 0-based
    bool playing = true;
    bool constructed = false;
    bool enabled = true;
    bool use_hand_cursor = true;
    bool tab_enabled = true, focus_enabled = false, tab_children = true;
    int tab_index = -1;
    MovieClip* mask = nullptr;       // setMask: the clip that masks this one
    MovieClip* mask_owner = nullptr; // setMask: the clip this one masks (it is not drawn on its own)
    DisplayObject* hit_area = nullptr;
    std::vector<uint32_t> init_actions_run; // sprites whose DoInitAction ran (root only)
    std::map<int, std::unique_ptr<DisplayObject>> children; // by depth, drawn in order
    std::unique_ptr<Drawing> drawing;                         // the drawing methods' content, if any
    // An engine image loaded into the clip: an
    // ImageRegistry id, drawn at (0, 0) at its size to GFx (64 x 64; GFx's default image 57 x 36) below the
    // clip's children; 0xFFFFFFFF: none.
    uint32_t engine_image = 0xFFFFFFFFu;

    uint32_t frame_count() const;
    DisplayObject* child_at(int depth) const;
    DisplayObject* child_named(std::string_view name) const;
    int next_highest_depth() const;
    // A new child from a character of this clip's movie (or `movie`), at a depth.
    DisplayObject* create_child(const MovieDef* movie, uint32_t character, int depth, std::string name,
                                uint32_t placed_by = osu::k_none);
    void remove_child(int depth);
    void swap_depths(DisplayObject* child, int new_depth);

    // Timeline: apply the place commands of `frame` (entering it in order).
    // Children a goto set aside because its target frame places them again (goto_frame), by depth.
    using TakenBack = std::map<int, std::unique_ptr<DisplayObject>>;
    void enter_frame_commands(uint32_t frame, TakenBack* taken = nullptr);
    // The timeline's objects as of frame `last` (0-based), depth by depth.
    std::map<int, TimelineSlot> timeline_state(uint32_t last) const;
    // Flash goto semantics: rebuild the timeline state of `frame` and keep matching objects.
    void goto_frame(uint32_t frame);

    bool local_bounds(float& x0, float& y0, float& x1, float& y1) const override;
    // Scale-9 grid of the sprite, in its own space (osu::k_none if none).
    uint32_t scale9() const;
};

// Text fields: properties and runs; layout and rendering live in text.cpp.
struct TextFormat {
    std::string font;
    double size = 12;
    uint32_t color = 0xFF000000u; // RGBA8
    bool bold = false, italic = false, underline = false;
    int align = 0; // 0 left, 1 right, 2 centre, 3 justify
    double left_margin = 0, right_margin = 0, indent = 0, leading = 0, letter_spacing = 0;
    // Whether letter spacing was given (HTML letterSpacing, a setTextFormat with it): only then does GFx write
    // LETTERSPACING into htmlText.
    bool letter_spacing_set = false;
    bool kerning = false;
    std::string url, target;
    // Which fields a partial TextFormat sets (setTextFormat with nulls).
    uint32_t set_mask = 0xFFFFFFFFu;
};

// An inline image of HTML text (Scaleform's <img src width height vspace hspace>): `src` names a bitmap the movie
// exports (its linkage name, as BitmapData.loadBitmap resolves it).
struct TextImage {
    std::string src;
    float width = 0, height = 0; // 0: the bitmap's own size
    float hspace = 0, vspace = 0;
};

struct TextRun {
    std::u32string text; // an image run holds one U+FFFC
    TextFormat format;
    std::shared_ptr<const TextImage> image;
};

class TextFieldObject final : public DisplayObject {
public:
    TextFieldObject(Player& player, const MovieDef* movie, uint32_t character);
    ~TextFieldObject() override;

    // Definition defaults, then script state.
    float width = 0, height = 0, origin_x = 0, origin_y = 0; // the field's box in its own space
    // The box auto-size starts from: as authored, or as script last set _width / _height.
    float base_x = 0, base_y = 0, base_width = 0, base_height = 0;
    bool html = false, multiline = false, word_wrap = false, password = false, selectable = true,
         editable = false, border = false, background = false, embed_fonts = true, condense_white = false,
         no_translate = false, auto_size_none = true;
    std::string auto_size = "none";      // none, left, right, center
    std::string text_auto_size = "none"; // Scaleform: none, shrink, fit
    std::string vertical_align = "none"; // Scaleform: none, top, center, bottom
    std::string vertical_auto_size = "none"; // Scaleform: none, top, center, bottom (the height follows the text)
    uint32_t border_color = 0xFF000000u, background_color = 0xFFFFFFFFu;
    int max_chars = 0;
    std::string variable;
    TextFormat default_format; // the record's; HTML set by script is read from it
    // Once GFx translated the field's authored HTML, text set as `text` takes the translated run's format instead of
    // the record's, until HTML is set.
    std::optional<TextFormat> translated_format;
    std::vector<TextRun> runs;
    int scroll = 1, hscroll = 0;
    int caret = 0, selection_begin = 0, selection_end = 0;

    std::u32string plain_text() const;
    void set_plain_text(std::u32string text);
    void set_html_text(std::string_view html);
    std::string html_text() const;

    // Layout cache (text.cpp); rebuilt when dirty.
    bool layout_dirty = true;
    std::unique_ptr<TextLayout> layout;
    const TextLayout& laid_out();
    float text_width();
    float text_height();

    bool local_bounds(float& x0, float& y0, float& x1, float& y1) const override;
};

class ButtonObject final : public DisplayObject {
public:
    ButtonObject(Player& player, const MovieDef* movie, uint32_t character);
    uint32_t button = 0;
    int state = 1; // 1 up, 2 over, 4 down
    std::vector<std::unique_ptr<DisplayObject>> parts; // for the current state
    std::vector<std::unique_ptr<DisplayObject>> hit_parts; // the hit-test state
    void set_state(int new_state);
    std::vector<std::unique_ptr<DisplayObject>> make_parts(int state_mask);
    bool local_bounds(float& x0, float& y0, float& x1, float& y1) const override;
};

std::u32string utf8_to_u32(std::string_view s);
std::string u32_to_utf8(std::u32string_view s);

} // namespace spud::ui::player
