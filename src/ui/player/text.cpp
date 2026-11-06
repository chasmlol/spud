#include "ui/player/text.hpp"

#include "ui/player/movie.hpp"
#include "ui/player/player.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>

namespace spud::ui::player {

namespace {

std::string lower(std::string_view s) {
    std::string o(s);
    std::transform(o.begin(), o.end(), o.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return o;
}

uint32_t parse_color(std::string_view v, uint32_t fallback) {
    if (!v.empty() && v[0] == '#') {
        v.remove_prefix(1);
    } else if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        v.remove_prefix(2);
    }
    uint32_t rgb = 0;
    if (v.empty() || std::from_chars(v.data(), v.data() + v.size(), rgb, 16).ec != std::errc()) {
        return fallback;
    }
    // The colour's alpha stays the one it replaces: a field whose DefineEditText colour has an alpha keeps it through
    // the movie's HTML (measured, run settings-alpha1: the Journal's Settings entries, 0x4C, stay dim after an
    // htmlText with COLOR="#FFFFFF", Text colour alpha).
    return ((rgb >> 16) & 0xFF) | (rgb & 0xFF00) | ((rgb & 0xFF) << 16) | (fallback & 0xFF000000u);
}

std::u32string decode_entities(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const size_t semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 8) {
            out += s[i];
            continue;
        }
        const std::string_view e = s.substr(i + 1, semi - i - 1);
        if (e == "amp") out += '&';
        else if (e == "lt") out += '<';
        else if (e == "gt") out += '>';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else if (e == "nbsp") out += "\xC2\xA0";
        else if (!e.empty() && e[0] == '#') {
            uint32_t cp = 0;
            const bool hex = e.size() > 1 && (e[1] == 'x' || e[1] == 'X');
            std::from_chars(e.data() + (hex ? 2 : 1), e.data() + e.size(), cp, hex ? 16 : 10);
            out += u32_to_utf8(std::u32string(1, char32_t(cp)));
        } else {
            out += s.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return utf8_to_u32(out);
}

struct Tag {
    std::string name;
    bool closing = false, self_closing = false;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::string attr(std::string_view n) const {
        for (const auto& [k, v] : attrs) {
            if (k == n) {
                return v;
            }
        }
        return {};
    }
    bool has(std::string_view n) const {
        return std::any_of(attrs.begin(), attrs.end(), [&](auto& a) { return a.first == n; });
    }
};

Tag parse_tag(std::string_view t) {
    Tag tag;
    size_t i = 0;
    if (i < t.size() && t[i] == '/') {
        tag.closing = true;
        ++i;
    }
    size_t start = i;
    while (i < t.size() && !std::isspace(uint8_t(t[i])) && t[i] != '/') {
        ++i;
    }
    tag.name = lower(t.substr(start, i - start));
    while (i < t.size()) {
        while (i < t.size() && std::isspace(uint8_t(t[i]))) {
            ++i;
        }
        if (i < t.size() && t[i] == '/') {
            tag.self_closing = true;
            ++i;
            continue;
        }
        start = i;
        while (i < t.size() && t[i] != '=' && !std::isspace(uint8_t(t[i])) && t[i] != '/') {
            ++i;
        }
        std::string key = lower(t.substr(start, i - start));
        std::string value;
        if (i < t.size() && t[i] == '=') {
            ++i;
            if (i < t.size() && (t[i] == '"' || t[i] == '\'')) {
                const char q = t[i++];
                start = i;
                while (i < t.size() && t[i] != q) {
                    ++i;
                }
                value = std::string(t.substr(start, i - start));
                ++i;
            } else {
                start = i;
                while (i < t.size() && !std::isspace(uint8_t(t[i]))) {
                    ++i;
                }
                value = std::string(t.substr(start, i - start));
            }
        }
        if (!key.empty()) {
            tag.attrs.emplace_back(std::move(key), std::move(value));
        }
    }
    return tag;
}

} // namespace

std::vector<TextRun> parse_html(std::string_view html, const TextFormat& base, bool condense_white, bool multiline) {
    std::vector<TextRun> runs;
    std::vector<TextFormat> stack{base};
    bool paragraph_open = false;
    // A single-line field takes no line breaks from HTML: <p>a</p> reads "a", <p>a</p><p>b</p> and a<br>b "ab"
    // (measured, run singleline1, the main menu's Header_tf; with multiline set first, <p>a</p> reads "a\r").
    // Text set as `text` keeps its breaks either way.
    const std::u32string brk = multiline ? std::u32string(U"\r") : std::u32string();
    auto emit = [&](std::u32string text) {
        if (text.empty()) {
            return;
        }
        if (!runs.empty() && !runs.back().image && runs.back().format.font == stack.back().font &&
            runs.back().format.size == stack.back().size &&
            runs.back().format.color == stack.back().color && runs.back().format.bold == stack.back().bold &&
            runs.back().format.italic == stack.back().italic && runs.back().format.align == stack.back().align &&
            runs.back().format.underline == stack.back().underline && runs.back().format.leading == stack.back().leading &&
            runs.back().format.letter_spacing == stack.back().letter_spacing) {
            runs.back().text += text;
        } else {
            runs.push_back({std::move(text), stack.back()});
        }
    };
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] == '<') {
            const size_t end = html.find('>', i);
            if (end == std::string_view::npos) {
                break;
            }
            const Tag tag = parse_tag(html.substr(i + 1, end - i - 1));
            i = end + 1;
            if (tag.name == "br" || tag.name == "sbr") {
                emit(brk);
                continue;
            }
            if (tag.name == "img" && !tag.closing) {
                // An inline image (an empty element: no content, no closing tag).
                auto im = std::make_shared<TextImage>();
                im->src = tag.attr("src");
                auto num = [&](const char* n) {
                    const std::string s = tag.attr(n);
                    float v = 0;
                    std::from_chars(s.data(), s.data() + s.size(), v);
                    return v;
                };
                im->width = num("width");
                im->height = num("height");
                im->hspace = num("hspace");
                im->vspace = num("vspace");
                runs.push_back({U"￼", stack.back(), std::move(im)});
                continue;
            }
            if (tag.closing) {
                if (tag.name == "p" || tag.name == "li") {
                    // The paragraph's end takes the format of its last character, not the bare <p> level's.
                    // Measured in the game twice: the Loading Menu's text (initial HTML <p><font
                    // letterSpacing="1">...</font></p> paragraphs, then the movie's SetText: getTextFormat, text =,
                    // setTextFormat) keeps letter spacing 1, and so do the HUD's subtitle and
                    // crosshair fields through their SetText. After an inline image
                    // the break takes the current format.
                    if (!runs.empty() && paragraph_open && !runs.back().image) {
                        stack.push_back(runs.back().format);
                        emit(brk);
                        stack.pop_back();
                    } else {
                        emit(brk);
                    }
                    paragraph_open = false;
                }
                if (stack.size() > 1 && tag.name != "br") {
                    stack.pop_back();
                }
                continue;
            }
            TextFormat f = stack.back();
            if (tag.name == "p") {
                if (paragraph_open) {
                    emit(brk);
                }
                paragraph_open = true;
                const std::string a = lower(tag.attr("align"));
                if (a == "left") f.align = 0;
                else if (a == "right") f.align = 1;
                else if (a == "center") f.align = 2;
                else if (a == "justify") f.align = 3;
            } else if (tag.name == "font") {
                if (tag.has("face")) f.font = tag.attr("face");
                if (tag.has("size")) {
                    const std::string s = tag.attr("size");
                    double v = 0;
                    std::from_chars(s.data() + ((s[0] == '+' || s[0] == '-') ? 1 : 0), s.data() + s.size(), v);
                    f.size = s[0] == '+' ? f.size + v : s[0] == '-' ? f.size - v : v;
                }
                if (tag.has("color")) f.color = parse_color(tag.attr("color"), f.color);
                if (tag.has("letterspacing")) {
                    // GFx keeps whole pixels: measured in the game, letterSpacing="6.500000" reads back 6 and
                    // "0.800000" 0, the
                    // Journal's "1.200000" fields 1 and the Stats Menu's 1.800000 1.
                    const std::string s = tag.attr("letterspacing");
                    double v = 0;
                    std::from_chars(s.data(), s.data() + s.size(), v);
                    f.letter_spacing = std::trunc(v);
                    f.letter_spacing_set = true;
                }
                if (tag.has("kerning")) f.kerning = tag.attr("kerning") == "1";
            } else if (tag.name == "b") {
                f.bold = true;
            } else if (tag.name == "i") {
                f.italic = true;
            } else if (tag.name == "u") {
                f.underline = true;
            } else if (tag.name == "a") {
                f.url = tag.attr("href");
                f.target = tag.attr("target");
            } else if (tag.name == "textformat") {
                auto num = [&](const char* n, double& out) {
                    if (tag.has(n)) {
                        const std::string s = tag.attr(n);
                        std::from_chars(s.data(), s.data() + s.size(), out);
                    }
                };
                num("leftmargin", f.left_margin);
                num("rightmargin", f.right_margin);
                num("indent", f.indent);
                num("leading", f.leading);
            }
            if (!tag.self_closing) {
                stack.push_back(f);
            }
            continue;
        }
        const size_t next = html.find('<', i);
        std::u32string text = decode_entities(html.substr(i, next == std::string_view::npos ? std::string_view::npos : next - i));
        i = next == std::string_view::npos ? html.size() : next;
        if (condense_white) {
            std::u32string c;
            bool space = false;
            for (char32_t ch : text) {
                const bool ws = ch == U' ' || ch == U'\n' || ch == U'\r' || ch == U'\t';
                if (ws) {
                    if (!space) {
                        c += U' ';
                    }
                    space = true;
                } else {
                    c += ch;
                    space = false;
                }
            }
            text = std::move(c);
        } else {
            for (auto& ch : text) {
                if (ch == U'\n') {
                    ch = U'\r';
                }
            }
        }
        emit(std::move(text));
    }
    // GFx keeps the break that closes the last paragraph (Flash Player drops it): measured in the game, htmlText
    // "<p>a</p>" reads back "a\r", "<p></p>" and the Journal's authored <p align="center"></p> "\r",
    // "<p>a</p><p>b</p>" "a\rb\r", while "a" and text = "a" read "a" (the game scenarios text-readback.osc,
    // text-readback2.osc). A newline the text itself ends with stays too (GFx reads a note set as "...Black-Briar
    // \n \n" back with 449 characters ending "\r \r" and 17 lines, Book Menu).
    return runs;
}

ResolvedImage resolve_image(const TextFieldObject& t, const TextImage& image) {
    ResolvedImage r;
    if (!ImageRegistry::name_of(image.src).empty()) {
        // An engine image (img://NAME): what the engine registered under NAME, 64 x 64 to GFx; for an
        // unregistered name GFx's own 57 x 36 default image (ProcessImageTags through
        // The game), laid out at that size even when the
        // cache lacks its pixels.
        const ImageRegistry& images = t.player.library.images;
        r.engine_image = images.resolve(image.src);
        r.found = r.engine_image != ImageRegistry::k_none;
        if (!r.found) {
            r.engine_image = images.default_image();
        }
        r.native_w = r.found ? ImageRegistry::k_size : ImageRegistry::k_missing_width;
        r.native_h = r.found ? ImageRegistry::k_size : ImageRegistry::k_missing_height;
    } else {
        // A bitmap the field's movie exports by that linkage name, else the root movie (as BitmapData.loadBitmap
        // resolves it).
        for (const MovieDef* m : {t.movie, t.player.movie.get()}) {
            const uint32_t ch = m ? m->export_named(image.src) : osu::k_none;
            if (ch != osu::k_none && m->data().characters[ch].kind == osu::CharKind::bitmap) {
                const osu::Bitmap& bm = m->data().bitmaps[m->data().characters[ch].index];
                r.movie = m;
                r.character = ch;
                r.found = true;
                r.native_w = float(bm.width);
                r.native_h = float(bm.height);
                break;
            }
        }
    }
    r.width = image.width > 0 ? image.width : r.native_w;
    r.height = image.height > 0 ? image.height : r.native_h;
    return r;
}

std::string runs_to_html(const std::vector<TextRun>& runs, const TextFieldObject* field) {
    // As GFx writes htmlText (measured over the game's display lists, 84236 paragraphs): each paragraph, one
    // per
    // "\r" (an empty one after the last "\r" left out), as <TEXTFORMAT INDENT LEFTMARGIN RIGHTMARGIN
    // LEADING><P ALIGN> and a <FONT FACE SIZE COLOR [LETTERSPACING] KERNING> per run, LETTERSPACING ("%f")
    // only where the format set it, <B>, <I>, <U> inside; an image as <IMG SRC WIDTH HEIGHT [VSPACE] [HSPACE]
    // ALIGN="baseline"> before the text, with the size it was laid out with when the field is given (run
    // images1), VSPACE before HSPACE whichever order they were set in (run images3: hspace='3' vspace='4' and
    // the other way round both read back VSPACE="4" HSPACE="3"); an empty paragraph keeps an empty <FONT>; no
    // text at all is "".
    auto escape = [](std::u32string_view s) {
        std::string o;
        for (char32_t c : s) {
            switch (c) {
            case U'<': o += "&lt;"; break;
            case U'>': o += "&gt;"; break;
            case U'&': o += "&amp;"; break;
            case U'"': o += "&quot;"; break;
            default: o += u32_to_utf8(std::u32string(1, c));
            }
        }
        return o;
    };
    static const char* aligns[] = {"LEFT", "RIGHT", "CENTER", "JUSTIFY"};
    auto font_open = [](const TextFormat& f) {
        const uint32_t c = f.color;
        std::string o = std::format("<FONT FACE=\"{}\" SIZE=\"{}\" COLOR=\"#{:02X}{:02X}{:02X}\"", f.font, f.size,
                                    c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF);
        if (f.letter_spacing_set) {
            o += std::format(" LETTERSPACING=\"{:.6f}\"", f.letter_spacing);
        }
        o += std::format(" KERNING=\"{}\">", f.kerning ? 1 : 0);
        if (f.bold) o += "<B>";
        if (f.italic) o += "<I>";
        if (f.underline) o += "<U>";
        return o;
    };
    auto font_close = [](const TextFormat& f) {
        std::string o;
        if (f.underline) o += "</U>";
        if (f.italic) o += "</I>";
        if (f.bold) o += "</B>";
        return o + "</FONT>";
    };
    struct Piece {
        const TextRun* run;
        std::u32string_view text;
    };
    std::string out;
    std::vector<Piece> para;
    const TextFormat* closing = nullptr; // the format of the "\r" that closes the paragraph
    auto flush = [&](bool closed) {
        if (para.empty() && !closed) {
            return;
        }
        const TextFormat& pf = !para.empty() ? para.front().run->format : *closing;
        out += std::format("<TEXTFORMAT INDENT=\"{}\" LEFTMARGIN=\"{}\" RIGHTMARGIN=\"{}\" LEADING=\"{}\"><P ALIGN=\"{}\">",
                           pf.indent, pf.left_margin, pf.right_margin, pf.leading, aligns[std::clamp(pf.align, 0, 3)]);
        bool text_written = false;
        for (const Piece& p : para) {
            if (p.run->image) {
                const TextImage& im = *p.run->image;
                float w = im.width, h = im.height;
                if (field) {
                    const ResolvedImage ri = resolve_image(*field, im);
                    w = ri.width;
                    h = ri.height;
                }
                out += std::format("<IMG SRC=\"{}\" WIDTH=\"{}\" HEIGHT=\"{}\"", im.src, w, h);
                if (im.vspace != 0) out += std::format(" VSPACE=\"{}\"", im.vspace);
                if (im.hspace != 0) out += std::format(" HSPACE=\"{}\"", im.hspace);
                out += " ALIGN=\"baseline\">";
                continue;
            }
            out += font_open(p.run->format) + escape(p.text) + font_close(p.run->format);
            text_written = true;
        }
        if (!text_written) {
            out += font_open(pf) + font_close(pf);
        }
        out += "</P></TEXTFORMAT>";
        para.clear();
    };
    for (const auto& r : runs) {
        if (r.image) {
            para.push_back({&r, {}});
            continue;
        }
        size_t start = 0;
        while (start <= r.text.size()) {
            const size_t br = r.text.find(U'\r', start);
            const std::u32string_view piece = std::u32string_view(r.text).substr(
                start, br == std::u32string::npos ? std::u32string::npos : br - start);
            if (!piece.empty()) {
                para.push_back({&r, piece});
            }
            if (br == std::u32string::npos) {
                break;
            }
            closing = &r.format;
            flush(true); // the paragraph this "\r" closes, empty or not
            start = br + 1;
        }
    }
    flush(false); // the text after the last "\r", if any
    return out;
}

void TextFieldObject::set_html_text(std::string_view h) {
    runs = parse_html(h, default_format, condense_white, multiline);
    // HTML replaces the translated text's format for later text too (measured, run text-readback: after an htmlText
    // of " ", Header_tf's SetText(" ") reads back in the record's format, no LETTERSPACING).
    translated_format.reset();
    layout_dirty = true;
}

std::string TextFieldObject::html_text() const {
    if (!html) {
        return u32_to_utf8(plain_text());
    }
    return runs_to_html(runs, this);
}

// ---- layout --------------------------------------------------------------------------------------

namespace {

struct Measured {
    FontRef font;
    const osu::Glyph* glyph = nullptr;
    float advance = 0;
};

const osu::Glyph* find_glyph(const FontRef& f, char32_t code) {
    if (!f) {
        return nullptr;
    }
    const auto& m = f.movie->data();
    const auto& font = m.fonts[f.font];
    const osu::Glyph* begin = m.glyphs.data() + font.first_glyph;
    const osu::Glyph* end = begin + font.glyph_count;
    const osu::Glyph* it = std::lower_bound(begin, end, uint32_t(code), [](const osu::Glyph& g, uint32_t c) { return g.code < c; });
    return it != end && it->code == code ? it : nullptr;
}

float kerning(const FontRef& f, char32_t a, char32_t b) {
    const auto& m = f.movie->data();
    const auto& font = m.fonts[f.font];
    for (uint32_t i = 0; i < font.kerning_count; ++i) {
        const auto& k = m.kernings[font.first_kerning + i];
        if (k.left == a && k.right == b) {
            return k.adjust;
        }
    }
    return 0;
}

// An inline image's left edge from the pen: none, its two extra pixels of advance come after it. Measured in
// the game's screenshots at 1.5 screen pixels a stage pixel: GFx's 57 x 36 default
// image in a left-aligned field at stage x 120 covers screen columns 183 to 268 exactly (stage 122.0 to
// 179.0: the field, its 2-pixel gutter, the image), run images4; at 26 x 26 in the Book Menu's Turn Pages
// label its edge is 2.28 past the field; the registered t_letter at 26 x 26 in the Take label correlates
// best 1.95 past its field (0.41, against 0.27 a screen pixel and a half either way), run images3.
constexpr float k_image_inset = 0.0f;

} // namespace

std::unique_ptr<TextLayout> layout_text(TextFieldObject& t) {
    auto out = std::make_unique<TextLayout>();
    struct Char {
        char32_t code;
        const TextFormat* format;
        FontRef font;
        const osu::Glyph* glyph;
        const TextImage* image = nullptr;
        const MovieDef* image_movie = nullptr; // the movie exporting the image's bitmap
        uint32_t image_character = osu::k_none;
        uint32_t engine_image = ImageRegistry::k_none; // or the engine image (img://)
        bool image_laid_out = false; // the image takes its box (a resolved one, or an unregistered img://)
        float image_w = 0, image_h = 0;
        float image_ascent = 0; // above the baseline, exact (field pixels)
    };
    std::vector<Char> chars;
    for (const auto& r : t.runs) {
        FontRef f = t.player.find_font(t.movie, r.format.font, r.format.bold, r.format.italic);
        if (r.image) {
            Char c{U'￼', &r.format, f, nullptr, r.image.get()};
            const ResolvedImage ri = resolve_image(t, *r.image);
            c.image_laid_out = ri.found || ri.native_h > 0;
            if (c.image_laid_out) {
                c.image_movie = ri.movie;
                c.image_character = ri.character;
                c.engine_image = ri.engine_image;
                c.image_w = ri.width;
                c.image_h = ri.height;
                // GFx stands the image on the baseline one row of its own pixels above its bottom, vspace rows
                // lower or higher, scaled with the image: ascent = height / own height x (own height - 1 + vspace).
                // Measured in the game (run images1, the Book movie's reference field, 22-pixel $SkyrimBooks text
                // with ascent 22.05 and descent 8.3): a 64 x 64 engine image gives ascent 63; heights 26, 40, 80
                // give 25.55, 39.35, 78.75; height 26 with vspace -5 and 10 gives 23.55 and 29.65; 64 with vspace
                // -5 gives 58; an unregistered 57 x 36 one 35 (each as GFx truncates it to whole twips, below).
                c.image_ascent = ri.height / ri.native_h * (ri.native_h - 1.0f + r.image->vspace);
            }
            chars.push_back(c);
            continue;
        }
        for (char32_t c : r.text) {
            const char32_t shown = t.password && c != U'\r' ? U'*' : c;
            chars.push_back({shown, &r.format, f, find_glyph(f, shown)});
        }
    }
    // An image's advance: its width and two pixels, whatever its hspace (measured, run images1: a 64-wide image's
    // character box is 66 wide with no hspace and with hspace 5, a 32-wide one 34; the next character starts there).
    // An image that resolves to nothing (an unknown exported bitmap) takes its hspace on both sides only.
    auto image_advance = [](const Char& c) {
        return !c.image ? 0.0f : c.image_laid_out ? c.image_w + 2.0f : 2.0f * c.image->hspace;
    };
    // A glyph's advance at a size, in whole twips, rounded to the nearest (measured, run glyph1: the 27 characters of a
    // mixed text in the main menu's Motd_tf, $EverywhereMediumFont 18, each getCharBoundaries box a whole number of
    // twips wide and the next starting there; rounding the font's advance gives all 27, flooring 11,
    // Text).
    auto glyph_advance = [](const Char& c, float size) { return std::round(c.glyph->advance * size * 20.0f) / 20.0f; };
    const float avail_w = std::max(0.0f, t.width - 2 * k_text_gutter);
    auto lay = [&](float scale) {
        out->glyphs.clear();
        out->images.clear();
        out->lines.clear();
        out->char_x.assign(chars.size(), 0.0f);
        out->char_advance.assign(chars.size(), 0.0f);
        out->scale = scale;
        float y = k_text_gutter;
        size_t i = 0;
        float max_w = 0;
        while (i <= chars.size()) {
            // One paragraph, then lines within it.
            size_t pend = i;
            while (pend < chars.size() && chars[pend].code != U'\r') {
                ++pend;
            }
            const TextFormat& pf = i < chars.size() ? *chars[i].format : (t.runs.empty() ? t.default_format : t.runs.back().format);
            const float lm = float(pf.left_margin), rm = float(pf.right_margin), indent = float(pf.indent);
            size_t ls = i;
            bool first_line = true;
            do {
                const float x0 = lm + (first_line ? indent : 0);
                const float width_limit = avail_w - lm - rm - (first_line ? indent : 0);
                size_t le = ls;
                float w = 0;
                size_t last_break = SIZE_MAX;
                float w_at_break = 0;
                size_t counted_end = SIZE_MAX; // w covers [ls, counted_end): a wrap leaves the breaking space out
                while (le < pend) {
                    const Char& c = chars[le];
                    const float size = float(c.format->size) * scale;
                    float adv = c.image ? image_advance(c) : c.glyph ? glyph_advance(c, size) : size * 0.25f;
                    adv += float(c.format->letter_spacing);
                    if (c.format->kerning && le + 1 < pend && c.font) {
                        adv += kerning(c.font, c.code, chars[le + 1].code) * size;
                    }
                    if (c.code == U' ') {
                        last_break = le;
                        w_at_break = w;
                    }
                    if (t.word_wrap && w + adv > width_limit && le > ls) {
                        if (last_break != SIZE_MAX && last_break > ls) {
                            le = last_break + 1;
                            w = w_at_break;
                            counted_end = last_break;
                        }
                        break;
                    }
                    w += adv;
                    ++le;
                }
                // Line metrics from the largest font on it.
                float ascent = 0, descent = 0, leading = 0;
                bool has_image = false;
                float image_ascent = 0, image_descent = 0;
                for (size_t k = ls; k < std::max(le, ls + 1) && k <= chars.size(); ++k) {
                    const Char* c = k < chars.size() ? &chars[k] : nullptr;
                    const TextFormat& f = c ? *c->format : pf;
                    const FontRef fr = c ? c->font : t.player.find_font(t.movie, f.font, f.bold, f.italic);
                    const float size = float(f.size) * scale;
                    if (fr) {
                        const auto& font = fr.movie->data().fonts[fr.font];
                        ascent = std::max(ascent, font.ascent * size);
                        descent = std::max(descent, font.descent * size);
                    } else {
                        ascent = std::max(ascent, size * 0.8f);
                        descent = std::max(descent, size * 0.2f);
                    }
                    leading = std::max(leading, float(f.leading));
                    if (c && c->image && c->image_laid_out) {
                        has_image = true;
                        image_ascent = std::max(image_ascent, c->image_ascent);
                        image_descent = std::max(image_descent, c->image_h - c->image_ascent);
                    }
                    if (!c) {
                        break;
                    }
                }
                // GFx keeps line metrics in whole twips: the ascent (the largest on the line, exact) rounded down, the
                // line's height (the largest ascent plus the largest descent) rounded; the descent is what is left.
                // Measured in the game, 22-pixel $SkyrimBooks (exact ascent
                // 22.086, descent 8.2715): "he" alone 22.05 + 8.3 = 30.35; under a 64-high image (ascent 63.0)
                // 63 + 8.25 = 71.25, under a 40-high one (39.375) 39.35 + 8.3 = 47.65, and the same for the other
                // ten texts measured. Earlier measurements agree: a 24-pixel line of the Journal's font reports
                // ascent 23.25 and descent 5.2, and the quest page's header below a two-line description sits at
                // 148.9.
                if (has_image) {
                    ascent = std::max(ascent, image_ascent);
                    descent = std::max(descent, image_descent);
                }
                {
                    const float at = ascent * 20.0f, dt = descent * 20.0f;
                    const float top = std::floor(at + 1e-3f);
                    ascent = top / 20.0f;
                    descent = (std::round(at + dt) - top) / 20.0f;
                }
                // Trailing spaces counted in w come off; the space a wrap broke at was never counted (measured in the
                // game: the Loading Menu's right-aligned, wrapped lines end where its last line does;
                // the Race Sex Menu's centred race descriptions centre every wrapped line on its words).
                float visible_w = w;
                for (size_t k = std::min(le, counted_end); k > ls && chars[k - 1].code == U' '; --k) {
                    const Char& c = chars[k - 1];
                    visible_w -= c.glyph ? glyph_advance(c, float(c.format->size) * scale) : 0;
                }
                // Alignment leaves out the letter spacing after the line's last glyph (textWidth keeps it):
                // measured in the game, the Tween Menu's right-aligned MAGIC (letterSpacing 6) sits 6 px and its
                // centred SKILLS 3 px further right than with it, SKILLS' textWidth 86 either way.
                float align_w = visible_w;
                // A line ending in an image is right-aligned and centred by its width without the image's two
                // extra pixels of advance, and reports that width (getLineMetrics); left-aligned it keeps
                // them. Measured in the game (runs images8, images9, capture-images4.osc, 200-wide fields):
                // GFx's 57-wide default image alone, left: width 59, x 0; right: 57, x 139; centred: 57,
                // x 69.5; after "he" (15): left 74, right 72 at x 124, centred 72 at 62; before "he", right:
                // 74 at 122; the HUD's 64-wide E left 66, right 64 at x 132.
                float image_trim = 0;
                for (size_t k = le; k > ls; --k) {
                    if (chars[k - 1].code != U' ') {
                        align_w -= float(chars[k - 1].format->letter_spacing);
                        if (chars[k - 1].image && chars[k - 1].image_laid_out &&
                            (pf.align == 1 || pf.align == 2)) {
                            image_trim = 2.0f;
                        }
                        break;
                    }
                }
                align_w -= image_trim;
                float x = x0;
                if (pf.align == 1) {
                    x = x0 + std::max(0.0f, width_limit - align_w);
                } else if (pf.align == 2) {
                    x = x0 + std::max(0.0f, (width_limit - align_w) * 0.5f);
                }
                TextLine line;
                line.y = y + ascent;
                line.x = x;
                line.align_shift = x - x0;
                line.width = visible_w - image_trim;
                line.ascent = ascent;
                line.descent = descent;
                line.leading = leading;
                line.first_char = uint32_t(ls);
                line.char_count = uint32_t(le - ls);
                float pen = k_text_gutter + x;
                for (size_t k = ls; k < le; ++k) {
                    const Char& c = chars[k];
                    const float size = float(c.format->size) * scale;
                    if (c.image) {
                        if (c.image_movie || c.engine_image != ImageRegistry::k_none) {
                            // The box from the image's own baseline (above), its left edge at the pen.
                            const float left = pen + k_image_inset, top = line.y - c.image_ascent;
                            out->images.push_back({c.image_movie, c.image_character, left, top, left + c.image_w,
                                                   top + c.image_h, c.engine_image});
                        }
                    } else if (c.glyph && c.font) {
                        out->glyphs.push_back({c.font.movie, uint32_t(c.glyph - c.font.movie->data().glyphs.data()), pen,
                                               line.y, size, c.format->color});
                    }
                    float adv = c.image ? image_advance(c) : c.glyph ? glyph_advance(c, size) : size * 0.25f;
                    adv += float(c.format->letter_spacing);
                    if (c.format->kerning && k + 1 < le && c.font) {
                        adv += kerning(c.font, c.code, chars[k + 1].code) * size;
                    }
                    out->char_x[k] = pen;
                    out->char_advance[k] = adv;
                    pen += adv;
                }
                out->lines.push_back(line);
                max_w = std::max(max_w, visible_w + x0);
                y += ascent + descent + leading;
                ls = le;
                while (t.word_wrap && ls < pend && chars[ls].code == U' ' && ls != le) {
                    ++ls;
                }
                first_line = false;
            } while (ls < pend);
            if (pend >= chars.size()) {
                break;
            }
            i = pend + 1;
        }
        out->text_width = max_w;
        // The empty line after a final break is a line (numLines) but adds no height: measured in GFx, a note
        // ending "\r \r" has 17 lines and the textHeight of the first 16.
        const size_t last = out->lines.size() > 1 && out->lines.back().char_count == 0 ? out->lines.size() - 2
                                                                                       : out->lines.size() - 1;
        out->text_height = out->lines.empty() ? 0 : out->lines[last].y + out->lines[last].descent - k_text_gutter;
    };
    lay(1.0f);
    // Scaleform's textAutoSize: shrink (only down) or fit (both ways) the fonts to the field.
    if ((t.text_auto_size == "shrink" || t.text_auto_size == "fit") && !chars.empty()) {
        const float avail_h = std::max(1.0f, t.height - 2 * k_text_gutter);
        float s = 1.0f;
        for (int iter = 0; iter < 12; ++iter) {
            const bool over = out->text_height > avail_h + 0.01f || (!t.word_wrap && out->text_width > avail_w + 0.01f);
            if (!over && (t.text_auto_size == "shrink" || iter > 0)) {
                break;
            }
            const float sh = out->text_height > 0 ? avail_h / out->text_height : 1.0f;
            const float sw = (!t.word_wrap && out->text_width > 0) ? avail_w / out->text_width : 1e9f;
            s = std::min(sh, sw) * s;
            if (t.text_auto_size == "shrink") {
                s = std::min(s, 1.0f);
            }
            lay(s);
        }
    }
    // Vertical alignment (Scaleform), within the field. A field with textAutoSize and no verticalAlign of its own is
    // centred: measured in the game on the main menu's news header (Header_tf, textAutoSize "shrink" only), whose text
    // sits 17.33 stage pixels below the top, half its free height (77.7 - 39 - 2 x 2) / 2 = 17.35.
    // Only single-line fields are centred. Measured: the item card's multiline effects text (PotionsLabel,
    // shrink, 65 high, 48 of text) stays at the top, where centring would put it 6.5 stage pixels lower
    // the HUD's notification line (multiline and wordWrap false, shrink) is centred, while the
    // Lockpicking Menu's LockLevelText and NumLockpicksText (multiline and wordWrap true, shrink, 57.7 high,
    // one 23-pixel line) sit at the top, as does the Race Sex Menu's PlayerName
    // Every measured multiline field also has wordWrap, so which of the two flags decides is not separated;
    // multiline "fit" is unmeasured.
    std::string_view va = t.vertical_align;
    if (va == "none" && !t.multiline && (t.text_auto_size == "shrink" || t.text_auto_size == "fit")) {
        va = "center";
    }
    if (va == "center" || va == "bottom") {
        const float free = t.height - 2 * k_text_gutter - out->text_height;
        const float dy = va == "center" ? free * 0.5f : free;
        for (auto& g : out->glyphs) {
            g.y += dy;
        }
        for (auto& im : out->images) {
            im.y0 += dy;
            im.y1 += dy;
        }
        for (auto& l : out->lines) {
            l.y += dy;
        }
    }
    return out;
}

} // namespace spud::ui::player
