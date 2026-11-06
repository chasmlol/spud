#pragma once

// Text field layout and HTML text (Flash 8 TextField; Scaleform's textAutoSize and verticalAlign).

#include "ui/player/display.hpp"

#include <memory>
#include <string>
#include <vector>

namespace spud::ui::player {

struct PlacedGlyph {
    const MovieDef* font_movie;
    uint32_t glyph; // index into the font movie's glyphs
    float x, y;     // pen position on the baseline, field space
    float size;     // pixels per em
    uint32_t color; // RGBA8
};

// An inline <img>: a bitmap character of `movie`, or an engine image (img://, `engine_image` an ImageRegistry id,
// `movie` null), its box in field space.
struct PlacedImage {
    const MovieDef* movie;
    uint32_t character;
    float x0, y0, x1, y1;
    uint32_t engine_image = 0xFFFFFFFFu;
};

struct TextLine {
    float y = 0;        // baseline
    float x = 0;        // the first character's offset from the text area's left edge (alignment, margins)
    // The part of x that is alignment (right, centre), which getCharBoundaries leaves out.
    float align_shift = 0;
    float width = 0;
    float ascent = 0, descent = 0, leading = 0;
    uint32_t first_char = 0, char_count = 0;
};

struct TextLayout {
    std::vector<PlacedGlyph> glyphs;
    std::vector<PlacedImage> images;
    std::vector<TextLine> lines;
    // Per character of the field's text: its pen position (field space) and advance, for getCharBoundaries.
    std::vector<float> char_x, char_advance;
    float text_width = 0, text_height = 0;
    float scale = 1; // textAutoSize: the factor fonts were scaled by
};

constexpr float k_text_gutter = 2.0f; // Flash text fields keep 2 pixels around the text

std::unique_ptr<TextLayout> layout_text(TextFieldObject& field);

// HTML <-> runs (the subset Flash text fields render), as GFx reads and writes htmlText (text.cpp).
std::vector<TextRun> parse_html(std::string_view html, const TextFormat& base, bool condense_white, bool multiline);
// `field`: the field the runs belong to, so images are written back with the size they were laid out with.
std::string runs_to_html(const std::vector<TextRun>& runs, const TextFieldObject* field = nullptr);

// What an inline <img src> of a field resolves to: an engine image (img://), else a bitmap
// a movie exports; its own size and the size it is laid out with (the width and height attributes, each defaulting
// to the image's own).
struct ResolvedImage {
    bool found = false;
    uint32_t engine_image = 0xFFFFFFFFu; // ImageRegistry id
    const MovieDef* movie = nullptr;     // or the exported bitmap
    uint32_t character = 0xFFFFFFFFu;
    float native_w = 0, native_h = 0;
    float width = 0, height = 0;
};
ResolvedImage resolve_image(const TextFieldObject& field, const TextImage& image);

} // namespace spud::ui::player
