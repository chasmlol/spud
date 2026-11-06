#pragma once

// Flash movies (SWF, and Scaleform's GFX variant) as Skyrim's interface uses them: file header, tag stream
// and the bit-packed records inside tags, after the public SWF 10 specification. Read only by the importer
// (convert/ui_import.cpp turns a movie into the native .osu format).

#include "core/binary.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace spud::legacy::swf {

enum Tag : uint16_t {
    tag_end = 0,
    tag_show_frame = 1,
    tag_define_shape = 2,
    tag_place_object = 4,
    tag_remove_object = 5,
    tag_define_bits = 6,
    tag_jpeg_tables = 8,
    tag_set_background_color = 9,
    tag_define_font = 10,
    tag_define_text = 11,
    tag_do_action = 12,
    tag_define_bits_lossless = 20,
    tag_define_bits_jpeg2 = 21,
    tag_define_shape2 = 22,
    tag_place_object2 = 26,
    tag_remove_object2 = 28,
    tag_define_shape3 = 32,
    tag_define_text2 = 33,
    tag_define_button2 = 34,
    tag_define_bits_jpeg3 = 35,
    tag_define_bits_lossless2 = 36,
    tag_define_edit_text = 37,
    tag_define_sprite = 39,
    tag_frame_label = 43,
    tag_define_morph_shape = 46,
    tag_define_font2 = 48,
    tag_export_assets = 56,
    tag_import_assets = 57,
    tag_do_init_action = 59,
    tag_place_object3 = 70,
    tag_import_assets2 = 71,
    tag_define_font3 = 75,
    tag_define_scaling_grid = 78,
    tag_define_shape4 = 83,
};

struct Header {
    std::string signature; // FWS, CWS, GFX or CFX
    uint8_t version = 0;
    float width = 0, height = 0; // pixels
    float frame_rate = 0;
    uint16_t frame_count = 0;
};

struct TagData {
    uint16_t code;
    std::span<const std::byte> data;
};

struct File {
    Header header;
    std::vector<std::byte> body; // decompressed, after the 8-byte file header
    std::vector<TagData> tags;   // the root timeline's tags (spans into body)
};

// Throws ParseError for anything that isn't a SWF/GFX movie.
File open(std::span<const std::byte> bytes);
std::vector<TagData> parse_tags(std::span<const std::byte> data);

// Bit-packed fields (SWF records are big-endian bit streams over little-endian bytes).
class BitReader {
public:
    explicit BitReader(std::span<const std::byte> data, size_t pos = 0) : m_data(data), m_pos(pos) {}
    uint32_t ubits(unsigned n);
    int32_t sbits(unsigned n);
    float fbits(unsigned n) { return float(sbits(n)) / 65536.0f; }
    void align() {
        if (m_bit != 0) {
            m_bit = 0;
            ++m_pos;
        }
    }
    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    int16_t s16() { return static_cast<int16_t>(u16()); }
    float fixed16() { return float(static_cast<int32_t>(u32())) / 65536.0f; } // 16.16
    float fixed8() { return float(s16()) / 256.0f; }                             // 8.8
    float f32();
    std::string cstr();
    std::span<const std::byte> take(size_t n);
    size_t pos() const { return m_pos; }
    size_t remaining() const { return m_data.size() - m_pos; }
    void seek(size_t pos) { m_pos = pos; m_bit = 0; }

private:
    std::span<const std::byte> m_data;
    size_t m_pos = 0;
    unsigned m_bit = 0; // bits consumed of m_data[m_pos]
};

// a, b, c, d, tx, ty with translation in twips.
struct Matrix {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
};
Matrix read_matrix(BitReader& r);

struct ColorTransform {
    float mul[4] = {1, 1, 1, 1};
    float add[4] = {0, 0, 0, 0}; // -255..255
};
ColorTransform read_cxform(BitReader& r, bool with_alpha);

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // twips
};
Rect read_rect(BitReader& r);

uint32_t read_rgb(BitReader& r);  // as RGBA8 with alpha 255
uint32_t read_rgba(BitReader& r); // RGBA8

struct GradientRecord {
    uint8_t ratio;
    uint32_t color;
};

struct FillStyle {
    uint8_t type = 0; // 0x00 solid, 0x10 linear, 0x12 radial, 0x13 focal, 0x40-0x43 bitmap
    uint32_t color = 0;
    Matrix matrix;
    uint8_t spread = 0, interpolation = 0;
    float focal = 0;
    std::vector<GradientRecord> gradient;
    uint16_t bitmap = 0;
};

struct LineStyle {
    float width = 0; // twips
    uint32_t color = 0;
    uint8_t start_cap = 0, end_cap = 0, join = 0; // 0 round, 1 none / bevel, 2 square / miter
    float miter_limit = 3;
    bool no_close = false;
    bool has_fill = false;
    FillStyle fill;
};

// One edge with the styles active when it was read. Style ids are global: index into Shape::fills/lines
// (+1), 0 = none.
struct Edge {
    int32_t x0, y0, cx, cy, x1, y1; // twips; straight edges have cx, cy at the midpoint
    bool curved;
    uint32_t fill0, fill1, line;
};

struct Shape {
    Rect bounds;
    std::vector<FillStyle> fills;
    std::vector<LineStyle> lines;
    std::vector<Edge> edges;
    // Styles are layered in groups (each "new styles" record starts one); fills and lines of a group are drawn
    // together, groups in order. group_fill_start[i] / group_line_start[i] index the first style of group i.
    std::vector<uint32_t> group_fill_start, group_line_start;
};

// version: 1-4 for DefineShape..DefineShape4; 0 for glyph shapes (no styles, fill 1 = glyph).
Shape read_shape(BitReader& r, int version);

struct Filter {
    uint8_t kind = 0; // 0 drop shadow, 1 blur, 2 glow, 3 bevel, 4 gradient glow, 5 convolution, 6 colour matrix,
                      // 7 gradient bevel
    uint32_t color = 0, highlight = 0;
    float blur_x = 0, blur_y = 0, strength = 1, angle = 0, distance = 0;
    uint8_t passes = 1;
    bool inner = false, knockout = false, composite = true, on_top = false;
    std::vector<float> values; // colour matrix / convolution
};
std::vector<Filter> read_filters(BitReader& r);

struct ClipAction {
    uint32_t events = 0;
    uint8_t key = 0;
    std::span<const std::byte> actions;
};

struct PlaceObject {
    int version = 2; // 1, 2 or 3
    bool move = false;
    uint16_t depth = 0;
    std::optional<uint16_t> character;
    std::optional<Matrix> matrix;
    std::optional<ColorTransform> cxform;
    std::optional<uint16_t> ratio;
    std::optional<std::string> name;
    std::optional<uint16_t> clip_depth;
    std::vector<Filter> filters;
    bool has_filters = false;
    std::optional<uint8_t> blend;
    std::optional<bool> cache_as_bitmap;
    std::optional<bool> visible;
    std::vector<ClipAction> clip_actions;
};
PlaceObject read_place_object(const TagData& tag, uint8_t swf_version);

struct EditText {
    uint16_t id = 0;
    Rect bounds;
    bool word_wrap = false, multiline = false, password = false, read_only = false, auto_size = false,
         no_select = false, border = false, was_static = false, html = false, use_outlines = false;
    std::optional<uint16_t> font;
    std::optional<std::string> font_class;
    float height = 0; // twips
    std::optional<uint32_t> color;
    uint16_t max_length = 0;
    bool has_layout = false;
    uint8_t align = 0;
    float left_margin = 0, right_margin = 0, indent = 0, leading = 0; // twips
    std::string variable;
    std::optional<std::string> text;
};
EditText read_edit_text(const TagData& tag);

struct Font {
    uint16_t id = 0;
    std::string name;
    bool bold = false, italic = false, small_text = false, has_layout = false;
    std::vector<Shape> glyphs;      // in DefineFont3 units (20480 per em)
    std::vector<uint16_t> codes;
    float ascent = 0, descent = 0, leading = 0;
    std::vector<float> advances;
    std::vector<Rect> bounds;
    struct Kern {
        uint16_t left, right;
        float adjust;
    };
    std::vector<Kern> kerning;
    float units_per_em = 20480; // 1024 for DefineFont/DefineFont2
};
Font read_font(const TagData& tag);

// RGBA8 premultiplied, top-down. jpeg_tables: the movie's JPEGTables tag for DefineBits.
struct Image {
    uint16_t id = 0;
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
};
Image read_bitmap(const TagData& tag, std::span<const std::byte> jpeg_tables);

struct ButtonRecord {
    uint8_t states = 0;
    uint16_t character = 0, depth = 0;
    Matrix matrix;
    ColorTransform cxform;
    std::vector<Filter> filters;
    uint8_t blend = 0;
};

struct Button {
    uint16_t id = 0;
    bool track_as_menu = false;
    std::vector<ButtonRecord> records;
    struct Action {
        uint16_t conditions;
        std::span<const std::byte> actions;
    };
    std::vector<Action> actions;
};
Button read_button2(const TagData& tag);

} // namespace spud::legacy::swf
