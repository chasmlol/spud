#pragma once

// .osu — spud UI movie, and the two small UI tables next to it. Spec: docs/formats/osu.md
//
// A movie is a compiled Flash-style movie: the importer has already tessellated every shape and glyph,
// decoded every bitmap and translated the script into resolved instructions, so the player (src/ui/player)
// only lays out, runs and draws. Coordinates are stage pixels (the source's twips / 20), y down.

#include "formats/container.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spud::osu {

inline constexpr container::FourCC k_kind_movie = container::fourcc("UIMV");
inline constexpr container::FourCC k_kind_strings = container::fourcc("UITX"); // translations
inline constexpr container::FourCC k_kind_fonts = container::fourcc("UIFM");   // font libraries and names
inline constexpr container::FourCC k_kind_controls = container::fourcc("UICT"); // key names and the control map
inline constexpr uint32_t k_version = 2;
inline constexpr uint32_t k_none = 0xFFFFFFFFu;

struct Rect {
    float x0, y0, x1, y1;
};
static_assert(sizeof(Rect) == 16);

// x' = a*x + c*y + tx, y' = b*x + d*y + ty.
struct Matrix {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
};
static_assert(sizeof(Matrix) == 24);

// c' = c * mul + add, per channel RGBA, add in 0..1.
struct ColorTransform {
    float mul[4] = {1, 1, 1, 1};
    float add[4] = {0, 0, 0, 0};
};
static_assert(sizeof(ColorTransform) == 32);

struct Header {
    float stage_width;
    float stage_height;
    float frame_rate;
    uint32_t root_sprite; // index into sprites: the main timeline
    uint32_t background;  // RGBA8
    uint32_t source_version;
    uint32_t reserved[2];
};
static_assert(sizeof(Header) == 32);

enum class CharKind : uint8_t { shape, sprite, text, button, bitmap, font, imported };

// A character by its source id; `index` points into the table of its kind.
struct Character {
    uint16_t id;
    CharKind kind;
    uint8_t pad;
    uint32_t index;
};
static_assert(sizeof(Character) == 8);

// ---- shapes ----------------------------------------------------------------------------------

enum class Fill : uint8_t { solid, linear, radial, focal, bitmap };

enum MeshFlag : uint8_t {
    mesh_bitmap_repeat = 1 << 0,
    mesh_bitmap_smooth = 1 << 1,
    mesh_stroke = 1 << 2,
};

// One fill or stroke of a shape: triangles plus how to colour them.
struct Mesh {
    Fill fill;
    uint8_t flags; // MeshFlag
    uint16_t pad;
    uint32_t color;    // solid: RGBA8 (straight alpha)
    uint32_t gradient; // index into gradients, or k_none
    uint32_t bitmap;   // character index of a bitmap, or k_none
    Matrix fill_matrix; // shape space -> gradient space ([-1, 1] square) or bitmap UV ([0, 1])
    uint32_t first_vertex, vertex_count;
    uint32_t first_index, index_count; // indices are relative to first_vertex
};
static_assert(sizeof(Mesh) == 56);

struct Shape {
    Rect bounds;
    uint32_t first_mesh, mesh_count;
};
static_assert(sizeof(Shape) == 24);

struct Vertex {
    float x, y;
};
static_assert(sizeof(Vertex) == 8);

enum class Spread : uint8_t { pad, reflect, repeat };

struct Gradient {
    Spread spread;
    uint8_t linear_rgb; // interpolate in linear RGB instead of sRGB
    uint8_t stop_count;
    uint8_t pad;
    float focal; // focal gradients: -1..1 along the x axis
    uint32_t first_stop;
};
static_assert(sizeof(Gradient) == 12);

struct GradientStop {
    float ratio; // 0..1
    uint32_t color; // RGBA8
};
static_assert(sizeof(GradientStop) == 8);

// RGBA8, premultiplied, rows top-down.
struct Bitmap {
    uint32_t width, height;
    uint64_t offset; // into the BDAT blob
};
static_assert(sizeof(Bitmap) == 16);

// ---- timelines -------------------------------------------------------------------------------

struct Sprite {
    uint32_t frame_count, first_frame;
    uint32_t first_label, label_count;
    uint32_t scale9; // index into rects, or k_none
    uint32_t pad;
};
static_assert(sizeof(Sprite) == 24);

struct Frame {
    uint32_t first_command, command_count; // place commands
    uint32_t first_action, action_count;   // indices into frame_actions (one per DoAction block)
    uint32_t first_init, init_count;       // init actions run when this frame is first reached
};
static_assert(sizeof(Frame) == 24);

// A DoAction block and where its tag sat in the frame: after `after` of the frame's place commands. Actions and
// the load events of placed children are queued in tag order.
struct FrameAction {
    uint32_t function;
    uint32_t after;
};
static_assert(sizeof(FrameAction) == 8);

struct Label {
    uint32_t sprite, frame, name;
};
static_assert(sizeof(Label) == 12);

struct InitAction {
    uint32_t sprite_character; // the sprite the block initialises (its class), as a character index
    uint32_t function;
};
static_assert(sizeof(InitAction) == 8);

enum PlaceFlag : uint32_t {
    place_move = 1 << 0,        // modify the object at depth (else: new object)
    place_character = 1 << 1,
    place_matrix = 1 << 2,
    place_cxform = 1 << 3,
    place_ratio = 1 << 4,
    place_name = 1 << 5,
    place_clip_depth = 1 << 6,  // this object masks depths (depth, clip_depth]
    place_filters = 1 << 7,
    place_blend = 1 << 8,
    place_cache_bitmap = 1 << 9,
    place_visible = 1 << 10,
    place_events = 1 << 11,
    place_remove = 1 << 16,     // RemoveObject: only depth is used
};

enum class Blend : uint8_t { normal, layer, multiply, screen, lighten, darken, difference, add, subtract,
                             invert, alpha, erase, overlay, hardlight };

struct PlaceCommand {
    uint32_t flags; // PlaceFlag
    uint16_t depth;
    uint16_t clip_depth;
    uint32_t character; // character index
    uint32_t name;      // string, or k_none
    Matrix matrix;
    ColorTransform cxform;
    uint16_t ratio;
    Blend blend;
    uint8_t visible;
    uint32_t first_filter, filter_count;
    uint32_t first_event, event_count;
};
static_assert(sizeof(PlaceCommand) == 92);

enum class FilterKind : uint8_t { drop_shadow, blur, glow, bevel, gradient_glow, convolution, color_matrix,
                                  gradient_bevel };

enum FilterFlag : uint8_t {
    filter_inner = 1 << 0,
    filter_knockout = 1 << 1,
    filter_composite = 1 << 2,
    filter_on_top = 1 << 3,
};

struct Filter {
    FilterKind kind;
    uint8_t passes;
    uint8_t flags; // FilterFlag
    uint8_t pad;
    uint32_t color;     // shadow / glow colour (RGBA8)
    uint32_t highlight; // bevel highlight
    float blur_x, blur_y;
    float strength;
    float angle;    // radians
    float distance; // pixels
    uint32_t first_value, value_count; // colour matrix (20) / gradient colours, into filter_values
};
static_assert(sizeof(Filter) == 40);

// A clip event handler (onClipEvent / on(...)) attached by a place command.
struct ClipEvent {
    uint32_t events; // source clip-event flags
    uint8_t key;
    uint8_t pad[3];
    uint32_t function;
};
static_assert(sizeof(ClipEvent) == 12);

// ---- text and fonts --------------------------------------------------------------------------

enum TextFlag : uint32_t {
    text_word_wrap = 1 << 0,
    text_multiline = 1 << 1,
    text_password = 1 << 2,
    text_read_only = 1 << 3,
    text_auto_size = 1 << 4,
    text_no_select = 1 << 5,
    text_border = 1 << 6,
    text_was_static = 1 << 7,
    text_html = 1 << 8,
    text_use_outlines = 1 << 9,
    text_has_color = 1 << 10,
    text_has_layout = 1 << 11,
};

struct EditText {
    Rect bounds;
    uint32_t flags; // TextFlag
    uint32_t font;  // character index (a font or an imported font), or k_none
    uint32_t font_class; // string, or k_none
    float height;   // pixels
    uint32_t color; // RGBA8
    uint16_t max_length;
    uint8_t align; // 0 left, 1 right, 2 centre, 3 justify
    uint8_t pad;
    float left_margin, right_margin, indent, leading;
    uint32_t variable;     // string, or k_none
    uint32_t initial_text; // string, or k_none
};
static_assert(sizeof(EditText) == 64);

enum FontFlag : uint32_t {
    font_bold = 1 << 0,
    font_italic = 1 << 1,
    font_small = 1 << 2,
};

// Metrics and glyph geometry in em units (1.0 = the font size), y down, origin on the baseline.
struct Font {
    uint32_t name;
    uint32_t flags; // FontFlag
    float ascent, descent, leading;
    uint32_t first_glyph, glyph_count; // glyphs sorted by code
    uint32_t first_kerning, kerning_count;
    uint32_t pad;
};
static_assert(sizeof(Font) == 40);

struct Glyph {
    uint32_t code; // Unicode
    float advance;
    Rect bounds;
    uint32_t first_vertex, vertex_count;
    uint32_t first_index, index_count;
};
static_assert(sizeof(Glyph) == 40);

struct Kerning {
    uint32_t left, right;
    float adjust;
};
static_assert(sizeof(Kerning) == 12);

// ---- buttons ---------------------------------------------------------------------------------

struct Button {
    uint32_t first_record, record_count;
    uint32_t first_action, action_count;
    uint32_t track_as_menu;
};
static_assert(sizeof(Button) == 20);

struct ButtonRecord {
    uint8_t states; // 1 up, 2 over, 4 down, 8 hit test
    Blend blend;
    uint16_t depth;
    uint32_t character;
    Matrix matrix;
    ColorTransform cxform;
    uint32_t first_filter, filter_count;
};
static_assert(sizeof(ButtonRecord) == 72);

struct ButtonAction {
    uint32_t conditions; // source condition flags (state transitions, key code in bits 9..15)
    uint32_t function;
};
static_assert(sizeof(ButtonAction) == 8);

// ---- exports and imports ---------------------------------------------------------------------

struct Export {
    uint32_t name;
    uint32_t character;
};
static_assert(sizeof(Export) == 8);

struct Import {
    uint32_t url;  // source movie, as written in the movie
    uint32_t name; // exported name there
};
static_assert(sizeof(Import) == 8);

// ---- script ----------------------------------------------------------------------------------
//
// Instructions keep ActionScript 2 (AVM1) opcode numbers and semantics. The translation resolves what the
// source encodes positionally: constant pools become string values, branch offsets become instruction
// indices, and each function body becomes its own Function. See docs/formats/osu.md for each op's operands.

enum class FunctionKind : uint8_t { block, function, function2 };

struct Function {
    uint32_t name; // string, or k_none
    uint32_t first_instruction, instruction_count;
    uint32_t first_param;
    uint16_t param_count;
    uint8_t registers;
    FunctionKind kind;
    uint16_t flags; // DefineFunction2 preload/suppress flags
    uint16_t pad;
};
static_assert(sizeof(Function) == 24);

struct Param {
    uint32_t name;
    uint32_t reg; // function2: register to store the argument in, 0 = by name only
};
static_assert(sizeof(Param) == 8);

struct Instruction {
    uint8_t op;
    uint8_t pad;
    uint16_t count; // Push: value count; GotoFrame2: play flag; GetURL2: method flags
    uint32_t arg;   // Push: first value; branches: target instruction; DefineFunction*: child function;
                    // StoreRegister: register; GotoFrame: frame; GoToLabel: string; GotoFrame2: scene bias
};
static_assert(sizeof(Instruction) == 8);

enum class ValueKind : uint8_t { string, number, null, undefined, reg, boolean };

struct PushValue {
    ValueKind kind;
    uint8_t pad[3];
    uint32_t u; // string offset, register or boolean
    double number;
};
static_assert(sizeof(PushValue) == 16);

// ---- the movie -------------------------------------------------------------------------------

struct Movie {
    Header header{};
    std::vector<char> strings; // STRS: NUL-terminated UTF-8, addressed by offset
    std::vector<Character> characters;
    std::vector<Shape> shapes;
    std::vector<Mesh> meshes;
    std::vector<Vertex> vertices; // shapes and glyphs
    std::vector<uint32_t> indices;
    std::vector<Gradient> gradients;
    std::vector<GradientStop> gradient_stops;
    std::vector<Bitmap> bitmaps;
    std::vector<std::byte> bitmap_data;
    std::vector<Sprite> sprites;
    std::vector<Frame> frames;
    std::vector<Label> labels;
    std::vector<FrameAction> frame_actions;
    std::vector<InitAction> init_actions;
    std::vector<PlaceCommand> commands;
    std::vector<Filter> filters;
    std::vector<float> filter_values;
    std::vector<ClipEvent> events;
    std::vector<Rect> rects; // scale-9 grids
    std::vector<EditText> texts;
    std::vector<Font> fonts;
    std::vector<Glyph> glyphs;
    std::vector<Kerning> kernings;
    std::vector<Button> buttons;
    std::vector<ButtonRecord> button_records;
    std::vector<ButtonAction> button_actions;
    std::vector<Export> exports;
    std::vector<Import> imports;
    std::vector<Function> functions;
    std::vector<Param> params;
    std::vector<Instruction> instructions;
    std::vector<PushValue> values;

    std::string_view string(uint32_t offset) const;
};

std::vector<std::byte> write(const Movie& movie);
Movie read(std::span<const std::byte> file);

// ---- translations: "$KEY" -> text ------------------------------------------------------------

struct Translation {
    uint32_t key, text; // string offsets; sorted by key
};
static_assert(sizeof(Translation) == 8);

struct Strings {
    std::vector<char> strings;
    std::vector<Translation> entries;
};

std::vector<std::byte> write(const Strings& table);
Strings read_strings(std::span<const std::byte> file);

// ---- fonts: which movies hold fonts, and logical names ("$EverywhereFont") -> font names -----

struct FontName {
    uint32_t logical; // string
    uint32_t font;    // string: the font's name in a library movie
    uint32_t flags;   // FontFlag
};
static_assert(sizeof(FontName) == 12);

struct FontMap {
    std::vector<char> strings;
    std::vector<uint32_t> libraries; // movie paths in the cache ("interface/fonts_en.osu")
    std::vector<FontName> names;
};

std::vector<std::byte> write(const FontMap& map);
FontMap read_font_map(std::span<const std::byte> file);

// ---- controls: key names and codes per device, and the control map's input contexts ---------

enum class InputDevice : uint8_t { keyboard, mouse, gamepad };

// A key of one device: keyboard keys by DirectInput scan code, mouse buttons and gamepad buttons by the ids the
// control map uses; `flash` is the Flash key code the menus see, k_no_flash if the key has none.
struct InputKey {
    uint32_t name;
    InputDevice device;
    uint8_t pad;
    uint16_t flash;
    uint32_t code;
};
static_assert(sizeof(InputKey) == 12);
inline constexpr uint16_t k_no_flash = 0xFFFF;

// One input context ("Main Gameplay", "Menu Mode", ...), numbered in file order.
struct ControlContext {
    uint32_t name;
    uint32_t first_binding, binding_count;
};
static_assert(sizeof(ControlContext) == 12);

// One key bound to a user event in a context (an event with two keyboard keys has two bindings).
struct ControlBinding {
    uint32_t event;
    InputDevice device;
    uint8_t remappable;
    uint16_t pad;
    uint32_t code;  // as in InputKey; a chord keeps its last key
    uint32_t flags; // the event's group flags (8th field), 0 if none
};
static_assert(sizeof(ControlBinding) == 16);

struct Controls {
    std::vector<char> strings;
    std::vector<InputKey> keys;
    std::vector<ControlContext> contexts;
    std::vector<ControlBinding> bindings;
};

std::vector<std::byte> write(const Controls& controls);
Controls read_controls(std::span<const std::byte> file);

} // namespace spud::osu
