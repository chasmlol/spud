// The display and system classes of the ActionScript 2 library (MovieClip, TextField, TextFormat, Button, Key,
// Mouse, Selection, Stage, System, Color, Date, AsBroadcaster, MovieClipLoader, flash.geom, flash.filters,
// flash.external.ExternalInterface, setInterval/setTimeout), per the ActionScript 2.0 Language Reference
// (Adobe, Flash 8), plus the Scaleform GFx 3.x extensions the game's movies use.

#include "core/log.hpp"
#include "core/platform.hpp"
#include "ui/player/player.hpp"

#include "ui/player/text.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <format>
#include <limits>
#include <numbers>

namespace spud::ui::player {

namespace {

constexpr double k_deg = 180.0 / std::numbers::pi;
constexpr double k_no_bounds = 6710886.35; // what getBounds reports for an empty clip (0x7FFFFFF twips)

DisplayObject* display_of(const Value& v) { return v.is_object() ? v.o->display : nullptr; }

MovieClip* clip_of(const Value& v) {
    DisplayObject* d = display_of(v);
    return d && d->kind == DisplayKind::clip ? static_cast<MovieClip*>(d) : nullptr;
}

TextFieldObject* text_of(const Value& v) {
    DisplayObject* d = display_of(v);
    return d && d->kind == DisplayKind::text ? static_cast<TextFieldObject*>(d) : nullptr;
}

void accessor(Vm& vm, Object* o, std::string_view name, NativeFn get, NativeFn set = nullptr) {
    Property& p = o->props.insert(name);
    p.value = Value{};
    p.getter = vm.make_function(std::move(get));
    p.setter = set ? vm.make_function(std::move(set)) : nullptr;
    p.flags = prop_dont_enum | prop_dont_delete;
}

Object* make_class(Vm& vm, Object* parent, std::string_view name, NativeFn ctor, Object* proto_parent = nullptr) {
    Object* proto = vm.make_object(proto_parent);
    Object* c = vm.make_function(std::move(ctor), proto);
    vm.define(parent, name, Value::object(c));
    return c;
}

Object* proto_of(Vm& vm, Object* ctor) { return vm.get(Value::object(ctor), "prototype").object_or_null(); }

double num(Vm& vm, const Value& v, double fallback) {
    if (v.is_null_or_undefined()) {
        return fallback;
    }
    const double d = vm.to_number(v);
    return std::isnan(d) ? fallback : d;
}

// TextFormat colours are 0xRRGGBB numbers; the player keeps RGBA8 (R in the low byte).
uint32_t rgb_to_rgba(double v) {
    const uint32_t c = uint32_t(int64_t(v)) & 0xFFFFFFu;
    return 0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
}
double rgba_to_rgb(uint32_t c) { return double(((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF)); }
uint32_t rgb_alpha(double rgb, double alpha) {
    const uint32_t a = uint32_t(std::clamp(alpha, 0.0, 1.0) * 255.0 + 0.5);
    return (rgb_to_rgba(rgb) & 0xFFFFFFu) | (a << 24);
}

const char* blend_names[] = {"normal", "layer", "multiply", "screen", "lighten", "darken", "difference", "add",
                             "subtract", "invert", "alpha", "erase", "overlay", "hardlight"};

std::string align_name(int a) { return a == 1 ? "right" : a == 2 ? "center" : a == 3 ? "justify" : "left"; }
int align_value(std::string_view s) { return s == "right" ? 1 : s == "center" ? 2 : s == "justify" ? 3 : 0; }

// ---- AsBroadcaster ---------------------------------------------------------------------------------

struct Broadcaster {
    Value add, remove, broadcast;
};

Broadcaster install_broadcaster(Vm& vm) {
    Object* ab = vm.make_object();
    vm.define(vm.global, "AsBroadcaster", Value::object(ab));
    auto listeners = [](NativeCall& c) -> Object* {
        const Value l = c.vm.get(c.self, "_listeners");
        return l.is_object() && l.o->kind == ObjKind::array ? l.o : nullptr;
    };
    Broadcaster b;
    b.add = Value::object(vm.make_function([listeners](NativeCall& c) -> Value {
        Object* l = listeners(c);
        if (!l) {
            return Value::boolean(false);
        }
        // Adding a listener twice keeps one entry (it moves to the end).
        std::erase_if(l->elements, [&](const Value& v) { return strict_equals(v, c.arg(0)); });
        l->elements.push_back(c.arg(0));
        return Value::boolean(true);
    }));
    b.remove = Value::object(vm.make_function([listeners](NativeCall& c) -> Value {
        Object* l = listeners(c);
        if (!l) {
            return Value::boolean(false);
        }
        for (size_t i = 0; i < l->elements.size(); ++i) {
            if (strict_equals(l->elements[i], c.arg(0))) {
                l->elements.erase(l->elements.begin() + ptrdiff_t(i));
                return Value::boolean(true);
            }
        }
        return Value::boolean(false);
    }));
    b.broadcast = Value::object(vm.make_function([listeners](NativeCall& c) -> Value {
        Object* l = listeners(c);
        if (!l || c.args.empty()) {
            return {};
        }
        const auto name = c.vm.to_string(c.arg(0));
        const std::vector<Value> args(c.args.begin() + 1, c.args.end());
        const std::vector<Value> targets = l->elements;
        for (const auto& t : targets) {
            c.vm.call_method(t, *name, args);
        }
        return Value::boolean(!targets.empty());
    }));
    vm.define(ab, "addListener", b.add);
    vm.define(ab, "removeListener", b.remove);
    vm.define(ab, "broadcastMessage", b.broadcast);
    vm.define_method(ab, "initialize", [b](NativeCall& c) -> Value {
        if (Object* o = c.arg(0).object_or_null()) {
            c.vm.define(o, "_listeners", Value::object(c.vm.make_array()));
            c.vm.define(o, "addListener", b.add);
            c.vm.define(o, "removeListener", b.remove);
            c.vm.define(o, "broadcastMessage", b.broadcast);
        }
        return {};
    });
    return b;
}

void make_broadcaster(Vm& vm, Object* o, const Broadcaster& b) {
    vm.define(o, "_listeners", Value::object(vm.make_array()));
    vm.define(o, "addListener", b.add);
    vm.define(o, "removeListener", b.remove);
    vm.define(o, "broadcastMessage", b.broadcast);
}

// ---- flash.geom helpers ----------------------------------------------------------------------------

struct Geom {
    Object* point = nullptr;
    Object* rectangle = nullptr;
    Object* matrix = nullptr;
    Object* color_transform = nullptr;
    Object* transform = nullptr;
};

Value make_point(Vm& vm, const Geom& g, double x, double y) {
    Object* o = vm.make_object(proto_of(vm, g.point));
    vm.set(Value::object(o), "x", Value::number(x));
    vm.set(Value::object(o), "y", Value::number(y));
    return Value::object(o);
}

Value make_rect(Vm& vm, const Geom& g, double x, double y, double w, double h) {
    Object* o = vm.make_object(proto_of(vm, g.rectangle));
    vm.set(Value::object(o), "x", Value::number(x));
    vm.set(Value::object(o), "y", Value::number(y));
    vm.set(Value::object(o), "width", Value::number(w));
    vm.set(Value::object(o), "height", Value::number(h));
    return Value::object(o);
}

Value make_matrix(Vm& vm, const Geom& g, const Matrix2D& m) {
    Object* o = vm.make_object(proto_of(vm, g.matrix));
    const Value v = Value::object(o);
    vm.set(v, "a", Value::number(m.a));
    vm.set(v, "b", Value::number(m.b));
    vm.set(v, "c", Value::number(m.c));
    vm.set(v, "d", Value::number(m.d));
    vm.set(v, "tx", Value::number(m.tx));
    vm.set(v, "ty", Value::number(m.ty));
    return v;
}

Matrix2D read_matrix(Vm& vm, const Value& v) {
    return {float(num(vm, vm.get(v, "a"), 1)), float(num(vm, vm.get(v, "b"), 0)), float(num(vm, vm.get(v, "c"), 0)),
            float(num(vm, vm.get(v, "d"), 1)), float(num(vm, vm.get(v, "tx"), 0)), float(num(vm, vm.get(v, "ty"), 0))};
}

Value make_color_transform(Vm& vm, const Geom& g, const ColorXform& x) {
    Object* o = vm.make_object(proto_of(vm, g.color_transform));
    const Value v = Value::object(o);
    const char* mul[4] = {"redMultiplier", "greenMultiplier", "blueMultiplier", "alphaMultiplier"};
    const char* add[4] = {"redOffset", "greenOffset", "blueOffset", "alphaOffset"};
    for (int i = 0; i < 4; ++i) {
        vm.set(v, mul[i], Value::number(x.mul[i]));
        vm.set(v, add[i], Value::number(x.add[i] * 255.0));
    }
    return v;
}

ColorXform read_color_transform(Vm& vm, const Value& v) {
    ColorXform x;
    const char* mul[4] = {"redMultiplier", "greenMultiplier", "blueMultiplier", "alphaMultiplier"};
    const char* add[4] = {"redOffset", "greenOffset", "blueOffset", "alphaOffset"};
    for (int i = 0; i < 4; ++i) {
        x.mul[i] = float(num(vm, vm.get(v, mul[i]), 1));
        x.add[i] = float(num(vm, vm.get(v, add[i]), 0) / 255.0);
    }
    return x;
}

// ---- text format helpers ---------------------------------------------------------------------------

const char* k_format_props[] = {"font",        "size",        "color",  "bold",    "italic",        "underline",
                                "url",         "target",      "align",  "leftMargin", "rightMargin", "indent",
                                "leading",     "blockIndent", "bullet", "tabStops", "kerning",      "letterSpacing",
                                "display"};

enum FormatBit : uint32_t {
    fmt_font = 1 << 0, fmt_size = 1 << 1, fmt_color = 1 << 2, fmt_bold = 1 << 3, fmt_italic = 1 << 4,
    fmt_underline = 1 << 5, fmt_url = 1 << 6, fmt_target = 1 << 7, fmt_align = 1 << 8, fmt_left = 1 << 9,
    fmt_right = 1 << 10, fmt_indent = 1 << 11, fmt_leading = 1 << 12, fmt_kerning = 1 << 13,
    fmt_letter_spacing = 1 << 14,
};

// The fields a TextFormat object sets (null and undefined leave a field alone).
TextFormat read_format(Vm& vm, const Value& f, uint32_t& mask) {
    TextFormat out;
    mask = 0;
    auto has = [&](const char* n, Value& v) {
        v = vm.get(f, n);
        return !v.is_null_or_undefined();
    };
    Value v;
    if (has("font", v)) { out.font = *vm.to_string(v); mask |= fmt_font; }
    if (has("size", v)) { out.size = vm.to_number(v); mask |= fmt_size; }
    if (has("color", v)) { out.color = rgb_to_rgba(vm.to_number(v)); mask |= fmt_color; }
    if (has("bold", v)) { out.bold = vm.to_boolean(v); mask |= fmt_bold; }
    if (has("italic", v)) { out.italic = vm.to_boolean(v); mask |= fmt_italic; }
    if (has("underline", v)) { out.underline = vm.to_boolean(v); mask |= fmt_underline; }
    if (has("url", v)) { out.url = *vm.to_string(v); mask |= fmt_url; }
    if (has("target", v)) { out.target = *vm.to_string(v); mask |= fmt_target; }
    if (has("align", v)) { out.align = align_value(*vm.to_string(v)); mask |= fmt_align; }
    if (has("leftMargin", v)) { out.left_margin = vm.to_number(v); mask |= fmt_left; }
    if (has("rightMargin", v)) { out.right_margin = vm.to_number(v); mask |= fmt_right; }
    if (has("indent", v)) { out.indent = vm.to_number(v); mask |= fmt_indent; }
    if (has("leading", v)) { out.leading = vm.to_number(v); mask |= fmt_leading; }
    if (has("kerning", v)) { out.kerning = vm.to_boolean(v); mask |= fmt_kerning; }
    if (has("letterSpacing", v)) { out.letter_spacing = vm.to_number(v); mask |= fmt_letter_spacing; }
    return out;
}

void merge_format(TextFormat& into, const TextFormat& f, uint32_t mask) {
    if (mask & fmt_font) into.font = f.font;
    if (mask & fmt_size) into.size = f.size;
    // A TextFormat's colour is RGB: the alpha stays the run's own (measured: setTextFormat(getTextFormat()) leaves
    // the Journal's dim Settings entries dim, Text colour alpha). textColor is opaque.
    if (mask & fmt_color) into.color = (f.color & 0x00FFFFFFu) | (into.color & 0xFF000000u);
    if (mask & fmt_bold) into.bold = f.bold;
    if (mask & fmt_italic) into.italic = f.italic;
    if (mask & fmt_underline) into.underline = f.underline;
    if (mask & fmt_url) into.url = f.url;
    if (mask & fmt_target) into.target = f.target;
    if (mask & fmt_align) into.align = f.align;
    if (mask & fmt_left) into.left_margin = f.left_margin;
    if (mask & fmt_right) into.right_margin = f.right_margin;
    if (mask & fmt_indent) into.indent = f.indent;
    if (mask & fmt_leading) into.leading = f.leading;
    if (mask & fmt_kerning) into.kerning = f.kerning;
    if (mask & fmt_letter_spacing) {
        into.letter_spacing = f.letter_spacing;
        into.letter_spacing_set = true;
    }
}

void write_format(Vm& vm, const Value& o, const TextFormat& f, uint32_t same) {
    auto put = [&](const char* n, uint32_t bit, Value v) { vm.set(o, n, (same & bit) ? v : Value::null()); };
    put("font", fmt_font, Value::string(f.font));
    put("size", fmt_size, Value::number(f.size));
    put("color", fmt_color, Value::number(rgba_to_rgb(f.color)));
    put("bold", fmt_bold, Value::boolean(f.bold));
    put("italic", fmt_italic, Value::boolean(f.italic));
    put("underline", fmt_underline, Value::boolean(f.underline));
    put("url", fmt_url, Value::string(f.url));
    put("target", fmt_target, Value::string(f.target));
    put("align", fmt_align, Value::string(align_name(f.align)));
    put("leftMargin", fmt_left, Value::number(f.left_margin));
    put("rightMargin", fmt_right, Value::number(f.right_margin));
    put("indent", fmt_indent, Value::number(f.indent));
    put("leading", fmt_leading, Value::number(f.leading));
    put("kerning", fmt_kerning, Value::boolean(f.kerning));
    // null where no format set it (measured, run textformat1: Motd_tf's getTextFormat and getNewTextFormat give
    // letterSpacing null, Header_tf's 0), so setTextFormat(getTextFormat) sets none.
    put("letterSpacing", f.letter_spacing_set ? fmt_letter_spacing : 0, Value::number(f.letter_spacing));
    vm.set(o, "blockIndent", Value::number(0));
    vm.set(o, "bullet", Value::boolean(false));
}

uint32_t same_fields(const TextFormat& a, const TextFormat& b) {
    uint32_t m = 0;
    if (a.font == b.font) m |= fmt_font;
    if (a.size == b.size) m |= fmt_size;
    if (a.color == b.color) m |= fmt_color;
    if (a.bold == b.bold) m |= fmt_bold;
    if (a.italic == b.italic) m |= fmt_italic;
    if (a.underline == b.underline) m |= fmt_underline;
    if (a.url == b.url) m |= fmt_url;
    if (a.target == b.target) m |= fmt_target;
    if (a.align == b.align) m |= fmt_align;
    if (a.left_margin == b.left_margin) m |= fmt_left;
    if (a.right_margin == b.right_margin) m |= fmt_right;
    if (a.indent == b.indent) m |= fmt_indent;
    if (a.leading == b.leading) m |= fmt_leading;
    if (a.kerning == b.kerning) m |= fmt_kerning;
    if (a.letter_spacing == b.letter_spacing) m |= fmt_letter_spacing;
    return m;
}

// Splits runs so that [begin, end) starts and ends on run boundaries; returns the run index range.
std::pair<size_t, size_t> split_runs(TextFieldObject* t, size_t begin, size_t end) {
    auto split_at = [&](size_t pos) -> size_t {
        size_t at = 0;
        for (size_t i = 0; i < t->runs.size(); ++i) {
            const size_t n = t->runs[i].text.size();
            if (pos == at) {
                return i;
            }
            if (pos < at + n) {
                TextRun tail{t->runs[i].text.substr(pos - at), t->runs[i].format};
                t->runs[i].text.resize(pos - at);
                t->runs.insert(t->runs.begin() + ptrdiff_t(i) + 1, std::move(tail));
                return i + 1;
            }
            at += n;
        }
        return t->runs.size();
    };
    const size_t b = split_at(begin);
    const size_t e = split_at(end);
    return {b, e};
}

void replace_text(Player& p, TextFieldObject* t, size_t begin, size_t end, std::u32string_view text,
                  bool append_with_new_format = false) {
    std::u32string all = t->plain_text();
    const bool appending = begin >= all.size();
    begin = std::min(begin, all.size());
    end = std::clamp(end, begin, all.size());
    // The inserted text takes the format of the character it replaces or follows (Flash 8 replaceText). Text that
    // replaceText adds at the end takes the field's new text format instead: measured in the game, the Crafting
    // Menu's UpdateIngredients sets each slot's colour with setNewTextFormat and appends the slot's name with
    // replaceText(length, length + 1, name); the names draw in that colour (grey, 0x777777, for a missing
    // ingredient), the "Requires: " before them in white.
    TextFormat f = t->default_format;
    size_t at = 0;
    for (const auto& r : t->runs) {
        if (append_with_new_format && appending) {
            break;
        }
        if (begin < at + r.text.size() || (begin > 0 && begin == at + r.text.size())) {
            f = r.format;
            if (begin < at + r.text.size()) {
                break;
            }
        }
        at += r.text.size();
    }
    auto [rb, re] = split_runs(t, begin, end);
    t->runs.erase(t->runs.begin() + ptrdiff_t(rb), t->runs.begin() + ptrdiff_t(re));
    std::u32string ins(text);
    for (auto& c : ins) {
        if (c == U'\n') {
            c = U'\r';
        }
    }
    t->runs.insert(t->runs.begin() + ptrdiff_t(rb), TextRun{std::move(ins), f});
    std::erase_if(t->runs, [](const TextRun& r) { return r.text.empty(); });
    p.text_changed(t);
}

// SPUD_UI_TRACE=text logs every text a script sets on a field, with the field's path.
bool trace_text() {
    static const bool on = platform::env("SPUD_UI_TRACE").value_or("") == "text";
    return on;
}

void set_field_text(Player& p, TextFieldObject* t, std::string_view text, bool as_html) {
    if (trace_text()) {
        log::info("ui text: {} = \"{}\"{}", p.target_path(t), text, as_html ? " (html)" : "");
    }
    // GFx passes text set by script through the translator unless the field has noTranslate.
    if (as_html && t->html) {
        t->set_html_text(text);
        if (!t->no_translate) {
            const std::string plain = u32_to_utf8(t->plain_text());
            const std::string tr = p.translate_text(plain);
            if (tr != plain) {
                TextFormat f = t->runs.empty() ? t->default_format : t->runs.front().format;
                t->runs.clear();
                t->runs.push_back({utf8_to_u32(tr), f});
                for (auto& c : t->runs.back().text) {
                    if (c == U'\n') {
                        c = U'\r';
                    }
                }
            }
        }
    } else {
        t->set_plain_text(utf8_to_u32(t->no_translate ? std::string(text) : p.translate_text(text)));
    }
    t->scroll = std::max(1, t->scroll);
    p.text_changed(t);
}

int max_scroll(TextFieldObject* t) {
    const TextLayout& l = t->laid_out();
    if (l.lines.empty()) {
        return 1;
    }
    const float view = t->height - 2 * k_text_gutter;
    const float top_last = l.lines.back().y + l.lines.back().descent;
    // The smallest first line that still shows the last line entirely.
    for (size_t s = 0; s < l.lines.size(); ++s) {
        const float top = l.lines[s].y - l.lines[s].ascent;
        if (top_last - top <= view + 0.01f) {
            return int(s) + 1;
        }
    }
    return int(l.lines.size());
}

int bottom_scroll(TextFieldObject* t) {
    const TextLayout& l = t->laid_out();
    if (l.lines.empty()) {
        return 1;
    }
    const size_t first = size_t(std::clamp(t->scroll, 1, int(l.lines.size())) - 1);
    const float top = l.lines[first].y - l.lines[first].ascent;
    size_t last = first;
    for (size_t i = first; i < l.lines.size(); ++i) {
        if (l.lines[i].y + l.lines[i].descent - top <= t->height - 2 * k_text_gutter + 0.01f) {
            last = i;
        }
    }
    return int(last) + 1;
}

// ---- filters ---------------------------------------------------------------------------------------

struct FilterClasses {
    Object* drop_shadow = nullptr;
    Object* blur = nullptr;
    Object* glow = nullptr;
    Object* bevel = nullptr;
    Object* gradient_glow = nullptr;
    Object* gradient_bevel = nullptr;
    Object* color_matrix = nullptr;
    Object* convolution = nullptr;
};

Value make_filter_object(Vm& vm, const FilterClasses& fc, const osu::Filter& f, const float* values) {
    auto set = [&](Object* o, const char* n, Value v) { vm.set(Value::object(o), n, v); };
    auto rgb = [](uint32_t c) { return Value::number(rgba_to_rgb(c)); };
    auto alpha = [](uint32_t c) { return Value::number(double(c >> 24) / 255.0); };
    const double angle = double(f.angle) * k_deg;
    const double quality = f.passes;
    Object* o = nullptr;
    switch (f.kind) {
    case osu::FilterKind::drop_shadow:
        o = vm.make_object(proto_of(vm, fc.drop_shadow));
        set(o, "distance", Value::number(f.distance));
        set(o, "angle", Value::number(angle));
        set(o, "color", rgb(f.color));
        set(o, "alpha", alpha(f.color));
        set(o, "hideObject", Value::boolean(!(f.flags & osu::filter_composite)));
        break;
    case osu::FilterKind::glow:
        o = vm.make_object(proto_of(vm, fc.glow));
        set(o, "color", rgb(f.color));
        set(o, "alpha", alpha(f.color));
        break;
    case osu::FilterKind::blur:
        o = vm.make_object(proto_of(vm, fc.blur));
        break;
    case osu::FilterKind::bevel:
        o = vm.make_object(proto_of(vm, fc.bevel));
        set(o, "distance", Value::number(f.distance));
        set(o, "angle", Value::number(angle));
        set(o, "highlightColor", rgb(f.highlight));
        set(o, "highlightAlpha", alpha(f.highlight));
        set(o, "shadowColor", rgb(f.color));
        set(o, "shadowAlpha", alpha(f.color));
        set(o, "type", Value::string(f.flags & osu::filter_on_top ? "full" : f.flags & osu::filter_inner ? "inner" : "outer"));
        break;
    case osu::FilterKind::color_matrix: {
        o = vm.make_object(proto_of(vm, fc.color_matrix));
        std::vector<Value> m;
        for (uint32_t i = 0; i < f.value_count; ++i) {
            m.push_back(Value::number(values[f.first_value + i]));
        }
        set(o, "matrix", Value::object(vm.make_array(std::move(m))));
        return Value::object(o);
    }
    case osu::FilterKind::gradient_glow:
    case osu::FilterKind::gradient_bevel: {
        o = vm.make_object(proto_of(vm, f.kind == osu::FilterKind::gradient_glow ? fc.gradient_glow : fc.gradient_bevel));
        std::vector<Value> colors, alphas, ratios;
        for (uint32_t i = 0; i + 1 < f.value_count; i += 2) {
            uint32_t c;
            std::memcpy(&c, &values[f.first_value + i + 1], sizeof c);
            ratios.push_back(Value::number(std::round(values[f.first_value + i] * 255.0)));
            colors.push_back(rgb(c));
            alphas.push_back(alpha(c));
        }
        set(o, "distance", Value::number(f.distance));
        set(o, "angle", Value::number(angle));
        set(o, "colors", Value::object(vm.make_array(std::move(colors))));
        set(o, "alphas", Value::object(vm.make_array(std::move(alphas))));
        set(o, "ratios", Value::object(vm.make_array(std::move(ratios))));
        set(o, "type", Value::string(f.flags & osu::filter_on_top ? "full" : f.flags & osu::filter_inner ? "inner" : "outer"));
        break;
    }
    case osu::FilterKind::convolution:
        o = vm.make_object(proto_of(vm, fc.convolution));
        return Value::object(o);
    }
    set(o, "blurX", Value::number(f.blur_x));
    set(o, "blurY", Value::number(f.blur_y));
    set(o, "quality", Value::number(quality));
    if (f.kind != osu::FilterKind::blur) {
        set(o, "strength", Value::number(f.strength));
        set(o, "knockout", Value::boolean(f.flags & osu::filter_knockout));
        if (f.kind == osu::FilterKind::drop_shadow || f.kind == osu::FilterKind::glow) {
            set(o, "inner", Value::boolean(f.flags & osu::filter_inner));
        }
    }
    return Value::object(o);
}

bool read_filter_object(Vm& vm, const FilterClasses& fc, const Value& v, osu::Filter& f, std::vector<float>& values) {
    auto n = [&](const char* name, double d) { return num(vm, vm.get(v, name), d); };
    auto b = [&](const char* name) { return vm.to_boolean(vm.get(v, name)); };
    f = {};
    f.passes = uint8_t(std::clamp(n("quality", 1), 0.0, 15.0));
    f.blur_x = float(n("blurX", 4));
    f.blur_y = float(n("blurY", 4));
    f.strength = float(n("strength", 1));
    f.angle = float(n("angle", 45) / k_deg);
    f.distance = float(n("distance", 4));
    f.first_value = uint32_t(values.size());
    auto flags = [&](bool inner, bool on_top) {
        f.flags = uint8_t((inner ? osu::filter_inner : 0) | (b("knockout") ? osu::filter_knockout : 0) |
                          osu::filter_composite | (on_top ? osu::filter_on_top : 0));
    };
    if (vm.instance_of(v, fc.drop_shadow)) {
        f.kind = osu::FilterKind::drop_shadow;
        f.color = rgb_alpha(n("color", 0), n("alpha", 1));
        flags(b("inner"), false);
        if (b("hideObject")) {
            f.flags &= uint8_t(~osu::filter_composite);
        }
    } else if (vm.instance_of(v, fc.glow)) {
        f.kind = osu::FilterKind::glow;
        f.color = rgb_alpha(n("color", 0xFF0000), n("alpha", 1));
        f.blur_x = float(n("blurX", 6));
        f.blur_y = float(n("blurY", 6));
        f.strength = float(n("strength", 2));
        f.distance = 0;
        flags(b("inner"), false);
    } else if (vm.instance_of(v, fc.blur)) {
        f.kind = osu::FilterKind::blur;
        f.distance = 0;
    } else if (vm.instance_of(v, fc.bevel)) {
        f.kind = osu::FilterKind::bevel;
        f.color = rgb_alpha(n("shadowColor", 0), n("shadowAlpha", 1));
        f.highlight = rgb_alpha(n("highlightColor", 0xFFFFFF), n("highlightAlpha", 1));
        const auto type = *vm.to_string(vm.get(v, "type"));
        flags(type != "outer", type == "full");
    } else if (vm.instance_of(v, fc.color_matrix)) {
        f.kind = osu::FilterKind::color_matrix;
        const Value m = vm.get(v, "matrix");
        for (int i = 0; i < 20; ++i) {
            values.push_back(float(num(vm, vm.get(m, std::to_string(i)), (i % 6 == 0) ? 1 : 0)));
        }
        f.value_count = 20;
    } else if (vm.instance_of(v, fc.gradient_glow) || vm.instance_of(v, fc.gradient_bevel)) {
        f.kind = vm.instance_of(v, fc.gradient_glow) ? osu::FilterKind::gradient_glow : osu::FilterKind::gradient_bevel;
        const Value colors = vm.get(v, "colors"), alphas = vm.get(v, "alphas"), ratios = vm.get(v, "ratios");
        const size_t count = colors.is_object() ? colors.o->elements.size() : 0;
        for (size_t i = 0; i < count; ++i) {
            const uint32_t c = rgb_alpha(num(vm, vm.get(colors, std::to_string(i)), 0),
                                         num(vm, vm.get(alphas, std::to_string(i)), 1));
            values.push_back(float(num(vm, vm.get(ratios, std::to_string(i)), 0) / 255.0));
            float bits;
            std::memcpy(&bits, &c, sizeof bits);
            values.push_back(bits);
        }
        f.value_count = uint32_t(count * 2);
        const auto type = *vm.to_string(vm.get(v, "type"));
        flags(type != "outer", type == "full");
    } else {
        return false;
    }
    return true;
}

} // namespace

// ---- install -------------------------------------------------------------------------------------

void install_display_library(Player& p) {
    Vm& vm = p.vm;
    Object* g = vm.global;
    const Broadcaster bc = install_broadcaster(vm);

    // Packages the movies address by name.
    Object* flash = vm.make_object();
    vm.define(g, "flash", Value::object(flash));
    Object* pkg_geom = vm.make_object();
    Object* pkg_filters = vm.make_object();
    Object* pkg_external = vm.make_object();
    Object* pkg_display = vm.make_object();
    vm.define(flash, "geom", Value::object(pkg_geom));
    vm.define(flash, "filters", Value::object(pkg_filters));
    vm.define(flash, "external", Value::object(pkg_external));
    vm.define(flash, "display", Value::object(pkg_display));

    // ---- flash.display.BitmapData (Flash 8): only what the movies use, loadBitmap's size ------------
    // loadBitmap(linkageId) gives the exported bitmap of that name (the movies export their button art as "E.png",
    // "360_A.png", ...), or undefined; the HUD sizes its inline <img> prompts from the result's width and height.
    Object* bitmap_data = make_class(vm, pkg_display, "BitmapData", [](NativeCall& c) -> Value {
        c.vm.set(c.self, "width", Value::number(num(c.vm, c.arg(0), 0)));
        c.vm.set(c.self, "height", Value::number(num(c.vm, c.arg(1), 0)));
        return {};
    });
    vm.define_method(proto_of(vm, bitmap_data), "dispose", [](NativeCall&) -> Value { return {}; });
    vm.define_method(bitmap_data, "loadBitmap", [&p, bitmap_data](NativeCall& c) -> Value {
        const std::string id = *c.vm.to_string(c.arg(0));
        const MovieDef* m = p.movie.get();
        const uint32_t ch = m ? m->export_named(id) : osu::k_none;
        if (ch == osu::k_none || m->data().characters[ch].kind != osu::CharKind::bitmap) {
            return {};
        }
        const osu::Bitmap& bm = m->data().bitmaps[m->data().characters[ch].index];
        Object* o = c.vm.make_object(proto_of(c.vm, bitmap_data));
        c.vm.set(Value::object(o), "width", Value::number(bm.width));
        c.vm.set(Value::object(o), "height", Value::number(bm.height));
        return Value::object(o);
    });

    // ---- flash.geom ----------------------------------------------------------------------------
    Geom geom;
    geom.point = make_class(vm, pkg_geom, "Point", [](NativeCall& c) -> Value {
        c.vm.set(c.self, "x", Value::number(num(c.vm, c.arg(0), 0)));
        c.vm.set(c.self, "y", Value::number(num(c.vm, c.arg(1), 0)));
        return {};
    });
    {
        Object* pp = proto_of(vm, geom.point);
        auto xy = [](NativeCall& c, const Value& v, double& x, double& y) {
            x = num(c.vm, c.vm.get(v, "x"), 0);
            y = num(c.vm, c.vm.get(v, "y"), 0);
        };
        vm.define_method(pp, "add", [geom, xy](NativeCall& c) -> Value {
            double x0, y0, x1, y1;
            xy(c, c.self, x0, y0);
            xy(c, c.arg(0), x1, y1);
            return make_point(c.vm, geom, x0 + x1, y0 + y1);
        });
        vm.define_method(pp, "subtract", [geom, xy](NativeCall& c) -> Value {
            double x0, y0, x1, y1;
            xy(c, c.self, x0, y0);
            xy(c, c.arg(0), x1, y1);
            return make_point(c.vm, geom, x0 - x1, y0 - y1);
        });
        vm.define_method(pp, "clone", [geom, xy](NativeCall& c) -> Value {
            double x, y;
            xy(c, c.self, x, y);
            return make_point(c.vm, geom, x, y);
        });
        vm.define_method(pp, "equals", [xy](NativeCall& c) -> Value {
            double x0, y0, x1, y1;
            xy(c, c.self, x0, y0);
            xy(c, c.arg(0), x1, y1);
            return Value::boolean(x0 == x1 && y0 == y1);
        });
        vm.define_method(pp, "offset", [xy](NativeCall& c) -> Value {
            double x, y;
            xy(c, c.self, x, y);
            c.vm.set(c.self, "x", Value::number(x + num(c.vm, c.arg(0), 0)));
            c.vm.set(c.self, "y", Value::number(y + num(c.vm, c.arg(1), 0)));
            return {};
        });
        vm.define_method(pp, "normalize", [xy](NativeCall& c) -> Value {
            double x, y;
            xy(c, c.self, x, y);
            const double len = std::hypot(x, y), want = num(c.vm, c.arg(0), 1);
            if (len > 0) {
                c.vm.set(c.self, "x", Value::number(x / len * want));
                c.vm.set(c.self, "y", Value::number(y / len * want));
            }
            return {};
        });
        vm.define_method(pp, "toString", [xy](NativeCall& c) -> Value {
            double x, y;
            xy(c, c.self, x, y);
            return Value::string(std::format("(x={}, y={})", Vm::number_to_string(x), Vm::number_to_string(y)));
        });
        accessor(vm, pp, "length", [xy](NativeCall& c) -> Value {
            double x, y;
            xy(c, c.self, x, y);
            return Value::number(std::hypot(x, y));
        });
        vm.define_method(geom.point, "distance", [xy](NativeCall& c) -> Value {
            double x0, y0, x1, y1;
            xy(c, c.arg(0), x0, y0);
            xy(c, c.arg(1), x1, y1);
            return Value::number(std::hypot(x1 - x0, y1 - y0));
        });
        vm.define_method(geom.point, "interpolate", [geom, xy](NativeCall& c) -> Value {
            double x0, y0, x1, y1;
            xy(c, c.arg(0), x0, y0);
            xy(c, c.arg(1), x1, y1);
            const double f = num(c.vm, c.arg(2), 0);
            return make_point(c.vm, geom, x1 + (x0 - x1) * f, y1 + (y0 - y1) * f);
        });
        vm.define_method(geom.point, "polar", [geom](NativeCall& c) -> Value {
            const double len = num(c.vm, c.arg(0), 0), a = num(c.vm, c.arg(1), 0);
            return make_point(c.vm, geom, len * std::cos(a), len * std::sin(a));
        });
    }
    geom.rectangle = make_class(vm, pkg_geom, "Rectangle", [](NativeCall& c) -> Value {
        c.vm.set(c.self, "x", Value::number(num(c.vm, c.arg(0), 0)));
        c.vm.set(c.self, "y", Value::number(num(c.vm, c.arg(1), 0)));
        c.vm.set(c.self, "width", Value::number(num(c.vm, c.arg(2), 0)));
        c.vm.set(c.self, "height", Value::number(num(c.vm, c.arg(3), 0)));
        return {};
    });
    {
        Object* rp = proto_of(vm, geom.rectangle);
        struct R {
            double x, y, w, h;
        };
        auto rd = [](NativeCall& c, const Value& v) {
            return R{num(c.vm, c.vm.get(v, "x"), 0), num(c.vm, c.vm.get(v, "y"), 0), num(c.vm, c.vm.get(v, "width"), 0),
                     num(c.vm, c.vm.get(v, "height"), 0)};
        };
        accessor(vm, rp, "left", [rd](NativeCall& c) -> Value { return Value::number(rd(c, c.self).x); },
                 [rd](NativeCall& c) -> Value {
                     const R r = rd(c, c.self);
                     const double l = num(c.vm, c.arg(0), 0);
                     c.vm.set(c.self, "width", Value::number(r.w + r.x - l));
                     c.vm.set(c.self, "x", Value::number(l));
                     return {};
                 });
        accessor(vm, rp, "top", [rd](NativeCall& c) -> Value { return Value::number(rd(c, c.self).y); },
                 [rd](NativeCall& c) -> Value {
                     const R r = rd(c, c.self);
                     const double t = num(c.vm, c.arg(0), 0);
                     c.vm.set(c.self, "height", Value::number(r.h + r.y - t));
                     c.vm.set(c.self, "y", Value::number(t));
                     return {};
                 });
        accessor(vm, rp, "right", [rd](NativeCall& c) -> Value { const R r = rd(c, c.self); return Value::number(r.x + r.w); },
                 [rd](NativeCall& c) -> Value {
                     c.vm.set(c.self, "width", Value::number(num(c.vm, c.arg(0), 0) - rd(c, c.self).x));
                     return {};
                 });
        accessor(vm, rp, "bottom", [rd](NativeCall& c) -> Value { const R r = rd(c, c.self); return Value::number(r.y + r.h); },
                 [rd](NativeCall& c) -> Value {
                     c.vm.set(c.self, "height", Value::number(num(c.vm, c.arg(0), 0) - rd(c, c.self).y));
                     return {};
                 });
        accessor(vm, rp, "topLeft", [geom, rd](NativeCall& c) -> Value { const R r = rd(c, c.self); return make_point(c.vm, geom, r.x, r.y); });
        accessor(vm, rp, "bottomRight", [geom, rd](NativeCall& c) -> Value {
            const R r = rd(c, c.self);
            return make_point(c.vm, geom, r.x + r.w, r.y + r.h);
        });
        accessor(vm, rp, "size", [geom, rd](NativeCall& c) -> Value { const R r = rd(c, c.self); return make_point(c.vm, geom, r.w, r.h); });
        vm.define_method(rp, "clone", [geom, rd](NativeCall& c) -> Value {
            const R r = rd(c, c.self);
            return make_rect(c.vm, geom, r.x, r.y, r.w, r.h);
        });
        vm.define_method(rp, "contains", [rd](NativeCall& c) -> Value {
            const R r = rd(c, c.self);
            const double x = num(c.vm, c.arg(0), 0), y = num(c.vm, c.arg(1), 0);
            return Value::boolean(x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h);
        });
        vm.define_method(rp, "containsPoint", [rd](NativeCall& c) -> Value {
            const R r = rd(c, c.self);
            const double x = num(c.vm, c.vm.get(c.arg(0), "x"), 0), y = num(c.vm, c.vm.get(c.arg(0), "y"), 0);
            return Value::boolean(x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h);
        });
        vm.define_method(rp, "isEmpty", [rd](NativeCall& c) -> Value { const R r = rd(c, c.self); return Value::boolean(r.w <= 0 || r.h <= 0); });
        vm.define_method(rp, "setEmpty", [](NativeCall& c) -> Value {
            for (const char* n : {"x", "y", "width", "height"}) {
                c.vm.set(c.self, n, Value::number(0));
            }
            return {};
        });
        vm.define_method(rp, "offset", [rd](NativeCall& c) -> Value {
            const R r = rd(c, c.self);
            c.vm.set(c.self, "x", Value::number(r.x + num(c.vm, c.arg(0), 0)));
            c.vm.set(c.self, "y", Value::number(r.y + num(c.vm, c.arg(1), 0)));
            return {};
        });
        vm.define_method(rp, "inflate", [rd](NativeCall& c) -> Value {
            const R r = rd(c, c.self);
            const double dx = num(c.vm, c.arg(0), 0), dy = num(c.vm, c.arg(1), 0);
            c.vm.set(c.self, "x", Value::number(r.x - dx));
            c.vm.set(c.self, "y", Value::number(r.y - dy));
            c.vm.set(c.self, "width", Value::number(r.w + 2 * dx));
            c.vm.set(c.self, "height", Value::number(r.h + 2 * dy));
            return {};
        });
        vm.define_method(rp, "intersects", [rd](NativeCall& c) -> Value {
            const R a = rd(c, c.self), b = rd(c, c.arg(0));
            return Value::boolean(a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h);
        });
        vm.define_method(rp, "intersection", [geom, rd](NativeCall& c) -> Value {
            const R a = rd(c, c.self), b = rd(c, c.arg(0));
            const double x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
            const double x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
            if (x1 <= x0 || y1 <= y0) {
                return make_rect(c.vm, geom, 0, 0, 0, 0);
            }
            return make_rect(c.vm, geom, x0, y0, x1 - x0, y1 - y0);
        });
        vm.define_method(rp, "union", [geom, rd](NativeCall& c) -> Value {
            const R a = rd(c, c.self), b = rd(c, c.arg(0));
            const double x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
            const double x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
            return make_rect(c.vm, geom, x0, y0, x1 - x0, y1 - y0);
        });
        vm.define_method(rp, "equals", [rd](NativeCall& c) -> Value {
            const R a = rd(c, c.self), b = rd(c, c.arg(0));
            return Value::boolean(a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h);
        });
        vm.define_method(rp, "toString", [rd](NativeCall& c) -> Value {
            const R r = rd(c, c.self);
            return Value::string(std::format("(x={}, y={}, w={}, h={})", Vm::number_to_string(r.x),
                                             Vm::number_to_string(r.y), Vm::number_to_string(r.w),
                                             Vm::number_to_string(r.h)));
        });
    }
    geom.matrix = make_class(vm, pkg_geom, "Matrix", [](NativeCall& c) -> Value {
        const char* n[6] = {"a", "b", "c", "d", "tx", "ty"};
        const double d[6] = {1, 0, 0, 1, 0, 0};
        for (int i = 0; i < 6; ++i) {
            c.vm.set(c.self, n[i], Value::number(num(c.vm, c.arg(size_t(i)), d[i])));
        }
        return {};
    });
    {
        Object* mp = proto_of(vm, geom.matrix);
        auto store = [](NativeCall& c, const Matrix2D& m) {
            c.vm.set(c.self, "a", Value::number(m.a));
            c.vm.set(c.self, "b", Value::number(m.b));
            c.vm.set(c.self, "c", Value::number(m.c));
            c.vm.set(c.self, "d", Value::number(m.d));
            c.vm.set(c.self, "tx", Value::number(m.tx));
            c.vm.set(c.self, "ty", Value::number(m.ty));
        };
        vm.define_method(mp, "identity", [store](NativeCall& c) -> Value { store(c, Matrix2D{}); return {}; });
        vm.define_method(mp, "clone", [geom](NativeCall& c) -> Value { return make_matrix(c.vm, geom, read_matrix(c.vm, c.self)); });
        vm.define_method(mp, "invert", [store](NativeCall& c) -> Value { store(c, read_matrix(c.vm, c.self).inverse()); return {}; });
        // Flash applies these after the existing matrix: m' = op * m.
        vm.define_method(mp, "concat", [store](NativeCall& c) -> Value {
            store(c, read_matrix(c.vm, c.arg(0)) * read_matrix(c.vm, c.self));
            return {};
        });
        vm.define_method(mp, "translate", [store](NativeCall& c) -> Value {
            Matrix2D m = read_matrix(c.vm, c.self);
            m.tx += float(num(c.vm, c.arg(0), 0));
            m.ty += float(num(c.vm, c.arg(1), 0));
            store(c, m);
            return {};
        });
        vm.define_method(mp, "scale", [store](NativeCall& c) -> Value {
            const float sx = float(num(c.vm, c.arg(0), 1)), sy = float(num(c.vm, c.arg(1), 1));
            store(c, Matrix2D{sx, 0, 0, sy, 0, 0} * read_matrix(c.vm, c.self));
            return {};
        });
        vm.define_method(mp, "rotate", [store](NativeCall& c) -> Value {
            const double a = num(c.vm, c.arg(0), 0);
            const float cs = float(std::cos(a)), sn = float(std::sin(a));
            store(c, Matrix2D{cs, sn, -sn, cs, 0, 0} * read_matrix(c.vm, c.self));
            return {};
        });
        vm.define_method(mp, "createBox", [store](NativeCall& c) -> Value {
            const double sx = num(c.vm, c.arg(0), 1), sy = num(c.vm, c.arg(1), 1), r = num(c.vm, c.arg(2), 0);
            const float cs = float(std::cos(r)), sn = float(std::sin(r));
            store(c, Matrix2D{float(sx) * cs, float(sx) * sn, -float(sy) * sn, float(sy) * cs,
                              float(num(c.vm, c.arg(3), 0)), float(num(c.vm, c.arg(4), 0))});
            return {};
        });
        vm.define_method(mp, "createGradientBox", [store](NativeCall& c) -> Value {
            // Maps the gradient square (-819.2..819.2) onto width x height at (tx, ty), rotated.
            const double w = num(c.vm, c.arg(0), 0), h = num(c.vm, c.arg(1), 0), r = num(c.vm, c.arg(2), 0);
            const double sx = w / 1638.4, sy = h / 1638.4;
            const float cs = float(std::cos(r)), sn = float(std::sin(r));
            store(c, Matrix2D{float(sx) * cs, float(sx) * sn, -float(sy) * sn, float(sy) * cs,
                              float(num(c.vm, c.arg(3), 0) + w / 2), float(num(c.vm, c.arg(4), 0) + h / 2)});
            return {};
        });
        vm.define_method(mp, "transformPoint", [geom](NativeCall& c) -> Value {
            const Matrix2D m = read_matrix(c.vm, c.self);
            float x, y;
            m.apply(float(num(c.vm, c.vm.get(c.arg(0), "x"), 0)), float(num(c.vm, c.vm.get(c.arg(0), "y"), 0)), x, y);
            return make_point(c.vm, geom, x, y);
        });
        vm.define_method(mp, "deltaTransformPoint", [geom](NativeCall& c) -> Value {
            Matrix2D m = read_matrix(c.vm, c.self);
            m.tx = m.ty = 0;
            float x, y;
            m.apply(float(num(c.vm, c.vm.get(c.arg(0), "x"), 0)), float(num(c.vm, c.vm.get(c.arg(0), "y"), 0)), x, y);
            return make_point(c.vm, geom, x, y);
        });
        vm.define_method(mp, "toString", [](NativeCall& c) -> Value {
            const Matrix2D m = read_matrix(c.vm, c.self);
            return Value::string(std::format("(a={}, b={}, c={}, d={}, tx={}, ty={})", Vm::number_to_string(m.a),
                                             Vm::number_to_string(m.b), Vm::number_to_string(m.c),
                                             Vm::number_to_string(m.d), Vm::number_to_string(m.tx),
                                             Vm::number_to_string(m.ty)));
        });
    }
    geom.color_transform = make_class(vm, pkg_geom, "ColorTransform", [](NativeCall& c) -> Value {
        const char* n[8] = {"redMultiplier", "greenMultiplier", "blueMultiplier", "alphaMultiplier",
                            "redOffset",     "greenOffset",     "blueOffset",     "alphaOffset"};
        for (int i = 0; i < 8; ++i) {
            c.vm.set(c.self, n[i], Value::number(num(c.vm, c.arg(size_t(i)), i < 4 ? 1 : 0)));
        }
        return {};
    });
    {
        Object* cp = proto_of(vm, geom.color_transform);
        accessor(vm, cp, "rgb",
                 [](NativeCall& c) -> Value {
                     const double r = num(c.vm, c.vm.get(c.self, "redOffset"), 0);
                     const double g2 = num(c.vm, c.vm.get(c.self, "greenOffset"), 0);
                     const double b = num(c.vm, c.vm.get(c.self, "blueOffset"), 0);
                     return Value::number(double((uint32_t(r) & 255) << 16 | (uint32_t(g2) & 255) << 8 | (uint32_t(b) & 255)));
                 },
                 [](NativeCall& c) -> Value {
                     const uint32_t v = uint32_t(int64_t(num(c.vm, c.arg(0), 0)));
                     c.vm.set(c.self, "redMultiplier", Value::number(0));
                     c.vm.set(c.self, "greenMultiplier", Value::number(0));
                     c.vm.set(c.self, "blueMultiplier", Value::number(0));
                     c.vm.set(c.self, "redOffset", Value::number((v >> 16) & 255));
                     c.vm.set(c.self, "greenOffset", Value::number((v >> 8) & 255));
                     c.vm.set(c.self, "blueOffset", Value::number(v & 255));
                     return {};
                 });
        vm.define_method(cp, "concat", [geom](NativeCall& c) -> Value {
            const ColorXform x = read_color_transform(c.vm, c.self) * read_color_transform(c.vm, c.arg(0));
            const Value tmp = make_color_transform(c.vm, geom, x);
            for (const char* n : {"redMultiplier", "greenMultiplier", "blueMultiplier", "alphaMultiplier", "redOffset",
                                  "greenOffset", "blueOffset", "alphaOffset"}) {
                c.vm.set(c.self, n, c.vm.get(tmp, n));
            }
            return {};
        });
    }
    geom.transform = make_class(vm, pkg_geom, "Transform", [](NativeCall& c) -> Value {
        if (Object* o = c.self.object_or_null()) {
            c.vm.define(o, "__target", c.arg(0));
        }
        return {};
    });
    {
        Object* tp = proto_of(vm, geom.transform);
        auto target = [](NativeCall& c) { return display_of(c.vm.get(c.self, "__target")); };
        accessor(vm, tp, "matrix",
                 [geom, target](NativeCall& c) -> Value {
                     DisplayObject* d = target(c);
                     return d ? make_matrix(c.vm, geom, d->matrix) : Value{};
                 },
                 [target](NativeCall& c) -> Value {
                     if (DisplayObject* d = target(c)) {
                         d->set_matrix(read_matrix(c.vm, c.arg(0)));
                         d->script_transformed = true;
                     }
                     return {};
                 });
        accessor(vm, tp, "concatenatedMatrix", [geom, target](NativeCall& c) -> Value {
            DisplayObject* d = target(c);
            return d ? make_matrix(c.vm, geom, d->world_matrix()) : Value{};
        });
        accessor(vm, tp, "colorTransform",
                 [geom, target](NativeCall& c) -> Value {
                     DisplayObject* d = target(c);
                     return d ? make_color_transform(c.vm, geom, d->cxform) : Value{};
                 },
                 [target](NativeCall& c) -> Value {
                     if (DisplayObject* d = target(c)) {
                         d->cxform = read_color_transform(c.vm, c.arg(0));
                         d->script_transformed = true;
                     }
                     return {};
                 });
        accessor(vm, tp, "concatenatedColorTransform", [geom, target](NativeCall& c) -> Value {
            DisplayObject* d = target(c);
            if (!d) {
                return {};
            }
            ColorXform x = d->cxform;
            for (const MovieClip* p2 = d->parent; p2; p2 = p2->parent) {
                x = p2->cxform * x;
            }
            return make_color_transform(c.vm, geom, x);
        });
        accessor(vm, tp, "pixelBounds", [geom, target](NativeCall& c) -> Value {
            DisplayObject* d = target(c);
            float x0, y0, x1, y1;
            if (!d || !d->bounds_in(nullptr, x0, y0, x1, y1)) {
                return make_rect(c.vm, geom, 0, 0, 0, 0);
            }
            return make_rect(c.vm, geom, x0, y0, x1 - x0, y1 - y0);
        });
    }

    // ---- flash.filters -------------------------------------------------------------------------
    FilterClasses fc;
    Object* bitmap_filter = make_class(vm, pkg_filters, "BitmapFilter", [](NativeCall&) -> Value { return {}; });
    Object* bf_proto = proto_of(vm, bitmap_filter);
    vm.define_method(bf_proto, "clone", [](NativeCall& c) -> Value {
        Object* o = c.vm.make_object(c.self.is_object() ? c.self.o->proto : nullptr);
        if (Object* s = c.self.object_or_null()) {
            for (const auto& k : c.vm.enumerate(s)) {
                Value v = c.vm.get(c.self, k);
                if (v.is_object() && v.o->kind == ObjKind::array) {
                    v = Value::object(c.vm.make_array(v.o->elements));
                }
                c.vm.set(Value::object(o), k, v);
            }
        }
        return Value::object(o);
    });
    // Constructor arguments and defaults per the Flash 8 ActionScript 2.0 Language Reference.
    auto filter_class = [&](const char* name, std::vector<std::pair<const char*, Value>> params) {
        return make_class(
            vm, pkg_filters, name,
            [params](NativeCall& c) -> Value {
                for (size_t i = 0; i < params.size(); ++i) {
                    Value v = c.arg(i);
                    if (v.is_undefined()) {
                        v = params[i].second;
                        if (v.is_object() && v.o->kind == ObjKind::array) {
                            v = Value::object(c.vm.make_array(v.o->elements));
                        }
                    }
                    c.vm.set(c.self, params[i].first, v);
                }
                return {};
            },
            bf_proto);
    };
    auto n = [](double d) { return Value::number(d); };
    auto bl = [](bool b) { return Value::boolean(b); };
    auto st = [](const char* s) { return Value::string(s); };
    fc.drop_shadow = filter_class("DropShadowFilter",
                                  {{"distance", n(4)}, {"angle", n(45)}, {"color", n(0)}, {"alpha", n(1)},
                                   {"blurX", n(4)}, {"blurY", n(4)}, {"strength", n(1)}, {"quality", n(1)},
                                   {"inner", bl(false)}, {"knockout", bl(false)}, {"hideObject", bl(false)}});
    fc.glow = filter_class("GlowFilter", {{"color", n(0xFF0000)}, {"alpha", n(1)}, {"blurX", n(6)}, {"blurY", n(6)},
                                          {"strength", n(2)}, {"quality", n(1)}, {"inner", bl(false)},
                                          {"knockout", bl(false)}});
    fc.blur = filter_class("BlurFilter", {{"blurX", n(4)}, {"blurY", n(4)}, {"quality", n(1)}});
    fc.bevel = filter_class("BevelFilter", {{"distance", n(4)}, {"angle", n(45)}, {"highlightColor", n(0xFFFFFF)},
                                            {"highlightAlpha", n(1)}, {"shadowColor", n(0)}, {"shadowAlpha", n(1)},
                                            {"blurX", n(4)}, {"blurY", n(4)}, {"strength", n(1)}, {"quality", n(1)},
                                            {"type", st("inner")}, {"knockout", bl(false)}});
    {
        std::vector<Value> identity;
        for (int i = 0; i < 20; ++i) {
            identity.push_back(Value::number(i % 6 == 0 ? 1 : 0));
        }
        Object* identity_array = vm.make_array(identity);
        p.pin(identity_array); // held by the constructor's defaults, which the collector cannot see
        fc.color_matrix = filter_class("ColorMatrixFilter", {{"matrix", Value::object(identity_array)}});
    }
    const Value empty_array = Value::object(vm.make_array());
    fc.gradient_glow = filter_class("GradientGlowFilter",
                                    {{"distance", n(4)}, {"angle", n(45)}, {"colors", empty_array}, {"alphas", empty_array},
                                     {"ratios", empty_array}, {"blurX", n(4)}, {"blurY", n(4)}, {"strength", n(1)},
                                     {"quality", n(1)}, {"type", st("inner")}, {"knockout", bl(false)}});
    fc.gradient_bevel = filter_class("GradientBevelFilter",
                                     {{"distance", n(4)}, {"angle", n(45)}, {"colors", empty_array}, {"alphas", empty_array},
                                      {"ratios", empty_array}, {"blurX", n(4)}, {"blurY", n(4)}, {"strength", n(1)},
                                      {"quality", n(1)}, {"type", st("inner")}, {"knockout", bl(false)}});
    fc.convolution = filter_class("ConvolutionFilter", {{"matrixX", n(0)}, {"matrixY", n(0)}, {"matrix", empty_array},
                                                        {"divisor", n(1)}, {"bias", n(0)}});
    filter_class("DisplacementMapFilter", {{"mapBitmap", Value::null()}, {"mapPoint", Value::null()},
                                           {"componentX", n(0)}, {"componentY", n(0)}, {"scaleX", n(0)},
                                           {"scaleY", n(0)}, {"mode", st("wrap")}, {"color", n(0)}, {"alpha", n(0)}});
    p.pin(empty_array.o);

    auto filters_getter = [fc](NativeCall& c) -> Value {
        DisplayObject* d = display_of(c.self);
        std::vector<Value> out;
        if (d) {
            for (const auto& f : d->filters) {
                out.push_back(make_filter_object(c.vm, fc, f, d->filter_values.data()));
            }
        }
        return Value::object(c.vm.make_array(std::move(out)));
    };
    auto filters_setter = [fc](NativeCall& c) -> Value {
        DisplayObject* d = display_of(c.self);
        if (!d) {
            return {};
        }
        d->filters.clear();
        d->filter_values.clear();
        const Value a = c.arg(0);
        if (a.is_object() && a.o->kind == ObjKind::array) {
            for (const auto& e : a.o->elements) {
                osu::Filter f;
                if (read_filter_object(c.vm, fc, e, f, d->filter_values)) {
                    d->filters.push_back(f);
                }
            }
        }
        return {};
    };
    auto blend_getter = [](NativeCall& c) -> Value {
        DisplayObject* d = display_of(c.self);
        return d ? Value::string(blend_names[size_t(d->blend) < std::size(blend_names) ? size_t(d->blend) : 0]) : Value{};
    };
    auto blend_setter = [](NativeCall& c) -> Value {
        DisplayObject* d = display_of(c.self);
        if (!d) {
            return {};
        }
        const Value v = c.arg(0);
        if (v.is_number()) {
            const int i = int(v.n) - 1; // 1 normal ... 14 hardlight
            if (i >= 0 && i < int(std::size(blend_names))) {
                d->blend = osu::Blend(i);
            }
        } else {
            const auto s = *c.vm.to_string(v);
            for (size_t i = 0; i < std::size(blend_names); ++i) {
                if (s == blend_names[i]) {
                    d->blend = osu::Blend(i);
                }
            }
        }
        return {};
    };

    // ---- MovieClip -----------------------------------------------------------------------------
    Object* mc_ctor = make_class(vm, g, "MovieClip", [](NativeCall&) -> Value { return {}; });
    Object* mc = proto_of(vm, mc_ctor);
    p.m_movieclip_proto = mc;
    vm.define(mc, "enabled", Value::boolean(true));
    vm.define(mc, "useHandCursor", Value::boolean(true));
    vm.define_method(mc, "toString", [](NativeCall& c) -> Value {
        DisplayObject* d = display_of(c.self);
        return Value::string(d ? d->target_path() : std::string());
    });
    auto goto_arg = [&p](NativeCall& c, bool play) -> Value {
        MovieClip* clip = clip_of(c.self);
        if (!clip) {
            return {};
        }
        const Value f = c.arg(0);
        if (f.is_string()) {
            if (p.goto_label(clip, *f.s, play)) {
                return {};
            }
            const double d = c.vm.to_number(f);
            if (std::isnan(d)) {
                return {};
            }
            p.goto_frame(clip, uint32_t(std::max(0.0, d - 1)), play);
            return {};
        }
        const double d = c.vm.to_number(f);
        if (!std::isnan(d)) {
            p.goto_frame(clip, uint32_t(std::max(0.0, std::floor(d) - 1)), play);
        }
        return {};
    };
    vm.define_method(mc, "gotoAndPlay", [goto_arg](NativeCall& c) -> Value { return goto_arg(c, true); });
    vm.define_method(mc, "gotoAndStop", [goto_arg](NativeCall& c) -> Value { return goto_arg(c, false); });
    vm.define_method(mc, "play", [&p](NativeCall& c) -> Value { p.play(display_of(c.self), true); return {}; });
    vm.define_method(mc, "stop", [&p](NativeCall& c) -> Value { p.play(display_of(c.self), false); return {}; });
    vm.define_method(mc, "nextFrame", [&p](NativeCall& c) -> Value { p.step(display_of(c.self), 1); return {}; });
    vm.define_method(mc, "prevFrame", [&p](NativeCall& c) -> Value { p.step(display_of(c.self), -1); return {}; });
    vm.define_method(mc, "getDepth", [](NativeCall& c) -> Value {
        DisplayObject* d = display_of(c.self);
        return d ? Value::number(d->depth) : Value{};
    });
    vm.define_method(mc, "getNextHighestDepth", [](NativeCall& c) -> Value {
        MovieClip* clip = clip_of(c.self);
        return clip ? Value::number(clip->next_highest_depth()) : Value{};
    });
    vm.define_method(mc, "getInstanceAtDepth", [](NativeCall& c) -> Value {
        MovieClip* clip = clip_of(c.self);
        DisplayObject* d = clip ? clip->child_at(int(num(c.vm, c.arg(0), 0))) : nullptr;
        return d && d->object ? Value::object(d->object) : Value{};
    });
    vm.define_method(mc, "getSWFVersion", [](NativeCall& c) -> Value {
        DisplayObject* d = display_of(c.self);
        return d && d->movie ? Value::number(d->movie->data().header.source_version) : Value::number(-1);
    });
    vm.define_method(mc, "getBytesLoaded", [](NativeCall& c) -> Value { return Value::number(display_of(c.self) ? 1 : 0); });
    vm.define_method(mc, "getBytesTotal", [](NativeCall& c) -> Value { return Value::number(display_of(c.self) ? 1 : 0); });
    vm.define_method(mc, "getURL", [](NativeCall&) -> Value { return {}; });
    vm.define_method(mc, "attachMovie", [&p](NativeCall& c) -> Value {
        MovieClip* clip = clip_of(c.self);
        if (!clip || c.args.size() < 3) {
            return {};
        }
        const auto id = c.vm.to_string(c.arg(0));
        const auto name = c.vm.to_string(c.arg(1));
        const double depth = c.vm.to_number(c.arg(2));
        if (std::isnan(depth)) {
            return {};
        }
        uint32_t ch = osu::k_none;
        const MovieDef* m = p.find_export(clip->movie, *id, ch);
        if (!m) {
            return {};
        }
        DisplayObject* obj = clip->create_child(m, ch, int(depth), *name);
        if (!obj) {
            return {};
        }
        p.construct_scripted(obj, c.arg(3));
        return obj->object ? Value::object(obj->object) : Value{};
    });
    vm.define_method(mc, "createEmptyMovieClip", [&p](NativeCall& c) -> Value {
        MovieClip* clip = clip_of(c.self);
        const double depth = c.vm.to_number(c.arg(1));
        if (!clip || std::isnan(depth)) {
            return {};
        }
        const int d = int(depth);
        clip->remove_child(d);
        auto child = std::make_unique<MovieClip>(p, clip->movie, osu::k_none, osu::k_none);
        child->parent = clip;
        child->depth = d;
        child->name = *c.vm.to_string(c.arg(0));
        MovieClip* raw = child.get();
        clip->children[d] = std::move(child);
        p.construct_scripted(raw, Value{});
        return Value::object(raw->object);
    });
    vm.define_method(mc, "createTextField", [&p](NativeCall& c) -> Value {
        MovieClip* clip = clip_of(c.self);
        const double depth = c.vm.to_number(c.arg(1));
        if (!clip || std::isnan(depth) || c.args.size() < 6) {
            return {};
        }
        const int d = int(depth);
        clip->remove_child(d);
        auto t = std::make_unique<TextFieldObject>(p, clip->movie, osu::k_none);
        t->parent = clip;
        t->depth = d;
        t->name = *c.vm.to_string(c.arg(0));
        t->matrix.tx = float(num(c.vm, c.arg(2), 0));
        t->matrix.ty = float(num(c.vm, c.arg(3), 0));
        t->width = t->base_width = float(std::max(0.0, num(c.vm, c.arg(4), 0)));
        t->height = t->base_height = float(std::max(0.0, num(c.vm, c.arg(5), 0)));
        // A new text field's format: Times New Roman 12, black, left (Flash 8 TextField).
        t->default_format.font = "Times New Roman";
        t->default_format.size = 12;
        t->embed_fonts = false;
        TextFieldObject* raw = t.get();
        clip->children[d] = std::move(t);
        p.construct_scripted(raw, Value{});
        return Value::object(raw->object);
    });
    vm.define_method(mc, "duplicateMovieClip", [&p](NativeCall& c) -> Value {
        MovieClip* clip = clip_of(c.self);
        const double depth = c.vm.to_number(c.arg(1));
        if (!clip || !clip->parent || std::isnan(depth)) {
            return {};
        }
        MovieClip* parent = clip->parent;
        const int d = int(depth);
        DisplayObject* obj = nullptr;
        if (clip->character == osu::k_none) {
            parent->remove_child(d);
            auto child = std::make_unique<MovieClip>(p, clip->movie, osu::k_none, osu::k_none);
            child->parent = parent;
            child->depth = d;
            obj = child.get();
            parent->children[d] = std::move(child);
            obj->name = *c.vm.to_string(c.arg(0));
        } else {
            obj = parent->create_child(clip->movie, clip->character, d, *c.vm.to_string(c.arg(0)));
        }
        if (!obj) {
            return {};
        }
        // The copy keeps the original's placement and clip events and starts on its first frame.
        obj->set_matrix(clip->matrix);
        obj->cxform = clip->cxform;
        obj->visible = clip->visible;
        obj->blend = clip->blend;
        obj->filters = clip->filters;
        obj->filter_values = clip->filter_values;
        obj->events_movie = clip->events_movie;
        obj->first_event = clip->first_event;
        obj->event_count = clip->event_count;
        obj->script_transformed = true;
        p.construct_scripted(obj, c.arg(2));
        return obj->object ? Value::object(obj->object) : Value{};
    });
    auto remove_self = [](NativeCall& c) -> Value {
        DisplayObject* d = display_of(c.self);
        // Only objects at script depths (0 .. 1048575) can be removed by script.
        if (d && d->parent && d->depth >= 0 && d->depth < 1048576) {
            d->parent->remove_child(d->depth);
        }
        return {};
    };
    vm.define_method(mc, "removeMovieClip", remove_self);
    vm.define_method(mc, "swapDepths", [](NativeCall& c) -> Value {
        DisplayObject* d = display_of(c.self);
        if (!d || !d->parent) {
            return {};
        }
        const Value t = c.arg(0);
        if (DisplayObject* other = display_of(t)) {
            if (other->parent == d->parent) {
                d->parent->swap_depths(d, other->depth);
            }
        } else {
            const double depth = c.vm.to_number(t);
            if (!std::isnan(depth)) {
                d->parent->swap_depths(d, int(depth));
            }
        }
        return {};
    });
    auto bounds_object = [](Vm& vm2, bool ok, float x0, float y0, float x1, float y1) {
        Object* o = vm2.make_object();
        const Value v = Value::object(o);
        vm2.set(v, "xMin", Value::number(ok ? x0 : k_no_bounds));
        vm2.set(v, "xMax", Value::number(ok ? x1 : k_no_bounds));
        vm2.set(v, "yMin", Value::number(ok ? y0 : k_no_bounds));
        vm2.set(v, "yMax", Value::number(ok ? y1 : k_no_bounds));
        return v;
    };
    auto get_bounds = [bounds_object](NativeCall& c) -> Value {
        DisplayObject* d = display_of(c.self);
        if (!d) {
            return {};
        }
        float x0, y0, x1, y1;
        bool ok;
        if (c.arg(0).is_undefined()) {
            ok = d->local_bounds(x0, y0, x1, y1);
        } else {
            DisplayObject* space = display_of(c.arg(0));
            if (!space) {
                return {};
            }
            ok = d->bounds_in(space, x0, y0, x1, y1);
        }
        return bounds_object(c.vm, ok, x0, y0, x1, y1);
    };
    vm.define_method(mc, "getBounds", get_bounds);
    vm.define_method(mc, "getRect", get_bounds);
    vm.define_method(mc, "globalToLocal", [](NativeCall& c) -> Value {
        DisplayObject* d = display_of(c.self);
        if (!d || !c.arg(0).is_object()) {
            return {};
        }
        float x, y;
        d->world_matrix().inverse().apply(float(num(c.vm, c.vm.get(c.arg(0), "x"), 0)),
                                          float(num(c.vm, c.vm.get(c.arg(0), "y"), 0)), x, y);
        c.vm.set(c.arg(0), "x", Value::number(x));
        c.vm.set(c.arg(0), "y", Value::number(y));
        return {};
    });
    vm.define_method(mc, "localToGlobal", [](NativeCall& c) -> Value {
        DisplayObject* d = display_of(c.self);
        if (!d || !c.arg(0).is_object()) {
            return {};
        }
        float x, y;
        d->world_matrix().apply(float(num(c.vm, c.vm.get(c.arg(0), "x"), 0)),
                                float(num(c.vm, c.vm.get(c.arg(0), "y"), 0)), x, y);
        c.vm.set(c.arg(0), "x", Value::number(x));
        c.vm.set(c.arg(0), "y", Value::number(y));
        return {};
    });
    auto hit_test = [](NativeCall& c) -> Value {
        DisplayObject* d = display_of(c.self);
        if (!d) {
            return Value::boolean(false);
        }
        if (DisplayObject* other = display_of(c.arg(0))) {
            float a0, a1, a2, a3, b0, b1, b2, b3;
            if (!d->bounds_in(nullptr, a0, a1, a2, a3) || !other->bounds_in(nullptr, b0, b1, b2, b3)) {
                return Value::boolean(false);
            }
            return Value::boolean(a0 <= b2 && b0 <= a2 && a1 <= b3 && b1 <= a3);
        }
        const float x = float(num(c.vm, c.arg(0), 0)), y = float(num(c.vm, c.arg(1), 0));
        return Value::boolean(d->hit_test_point(x, y, c.vm.to_boolean(c.arg(2))));
    };
    vm.define_method(mc, "hitTest", hit_test);
    vm.define_method(mc, "setMask", [](NativeCall& c) -> Value {
        MovieClip* clip = clip_of(c.self);
        if (!clip) {
            return {};
        }
        if (clip->mask) {
            clip->mask->mask_owner = nullptr;
            clip->mask = nullptr;
        }
        if (MovieClip* m = clip_of(c.arg(0)); m && m != clip) {
            if (m->mask_owner) {
                m->mask_owner->mask = nullptr;
            }
            clip->mask = m;
            m->mask_owner = clip;
        }
        return {};
    });
    vm.define_method(mc, "startDrag", [&p](NativeCall& c) -> Value {
        MovieClip* clip = clip_of(c.self);
        if (!clip) {
            return {};
        }
        const bool constrained = c.args.size() >= 5;
        p.start_drag(clip, c.vm.to_boolean(c.arg(0)), constrained, float(num(c.vm, c.arg(1), 0)),
                     float(num(c.vm, c.arg(2), 0)), float(num(c.vm, c.arg(3), 0)), float(num(c.vm, c.arg(4), 0)));
        return {};
    });
    vm.define_method(mc, "stopDrag", [&p](NativeCall&) -> Value { p.stop_drag(); return {}; });
    vm.define_method(mc, "loadMovie", [&p](NativeCall& c) -> Value {
        if (MovieClip* clip = clip_of(c.self)) {
            p.queue_load(clip, *c.vm.to_string(c.arg(0)), nullptr, nullptr);
        }
        return {};
    });
    vm.define_method(mc, "unloadMovie", [](NativeCall& c) -> Value {
        if (MovieClip* clip = clip_of(c.self)) {
            std::vector<int> depths;
            for (const auto& [d, ch] : clip->children) {
                depths.push_back(d);
            }
            for (int d : depths) {
                clip->remove_child(d);
            }
            clip->sprite = osu::k_none;
            clip->engine_image = ImageRegistry::k_none;
        }
        return {};
    });
    vm.define_method(mc, "attachAudio", [](NativeCall&) -> Value { return {}; });
    vm.define_method(mc, "attachBitmap", [](NativeCall&) -> Value { return {}; });
    // The drawing API (ActionScript 2.0 Language Reference, Flash 8, MovieClip): solid fills and strokes, kept in
    // clip space and drawn below the clip's children (Player::draw_drawing).
    auto drawing_of = [](NativeCall& c) -> Drawing* {
        MovieClip* clip = clip_of(c.self);
        if (!clip) {
            return nullptr;
        }
        if (!clip->drawing) {
            clip->drawing = std::make_unique<Drawing>();
        }
        return clip->drawing.get();
    };
    // "If an open path exists ... and it has a fill associated with it, that path is closed with a line and then
    // filled" (beginFill, endFill).
    auto close_fill = [](Drawing& d) {
        if (!d.filling) {
            return;
        }
        if (d.pen.x != d.contour_start.x || d.pen.y != d.contour_start.y) {
            if (d.has_line) {
                d.strokes.push_back({d.pen, d.contour_start, d.line_width, d.line_color});
            }
            if (!d.fill.contours.empty()) {
                d.fill.contours.back().push_back(d.contour_start);
            }
            d.pen = d.contour_start;
        }
        d.fills.push_back(std::move(d.fill));
        d.fill = {};
        d.filling = false;
    };
    auto line_to = [](Drawing& d, Drawing::Point to) {
        if (d.has_line) {
            d.strokes.push_back({d.pen, to, d.line_width, d.line_color});
        }
        if (d.filling) {
            if (d.fill.contours.empty()) {
                d.fill.contours.push_back({d.pen});
            }
            d.fill.contours.back().push_back(to);
        }
        d.pen = to;
    };
    vm.define_method(mc, "clear", [](NativeCall& c) -> Value {
        // Removes everything drawn, line style included.
        if (MovieClip* clip = clip_of(c.self)) {
            clip->drawing.reset();
        }
        return {};
    });
    vm.define_method(mc, "lineStyle", [drawing_of](NativeCall& c) -> Value {
        Drawing* d = drawing_of(c);
        if (!d) {
            return {};
        }
        // Thickness 0..255 (0 a hairline, none: no line); colour default black; alpha 0..100, default 100.
        d->has_line = !c.arg(0).is_null_or_undefined();
        d->line_width = float(std::clamp(num(c.vm, c.arg(0), 0), 0.0, 255.0));
        d->line_color = rgb_alpha(num(c.vm, c.arg(1), 0), num(c.vm, c.arg(2), 100) / 100.0);
        return {};
    });
    vm.define_method(mc, "moveTo", [drawing_of](NativeCall& c) -> Value {
        if (Drawing* d = drawing_of(c)) {
            d->pen = {float(num(c.vm, c.arg(0), 0)), float(num(c.vm, c.arg(1), 0))};
            if (d->filling) {
                d->fill.contours.push_back({d->pen});
                d->contour_start = d->pen;
            }
        }
        return {};
    });
    vm.define_method(mc, "lineTo", [drawing_of, line_to](NativeCall& c) -> Value {
        if (Drawing* d = drawing_of(c)) {
            line_to(*d, {float(num(c.vm, c.arg(0), 0)), float(num(c.vm, c.arg(1), 0))});
        }
        return {};
    });
    vm.define_method(mc, "curveTo", [drawing_of, line_to](NativeCall& c) -> Value {
        Drawing* d = drawing_of(c);
        if (!d) {
            return {};
        }
        // A quadratic Bezier from the pen through the control point to the anchor, in short straight pieces.
        const Drawing::Point p0 = d->pen;
        const Drawing::Point p1{float(num(c.vm, c.arg(0), 0)), float(num(c.vm, c.arg(1), 0))};
        const Drawing::Point p2{float(num(c.vm, c.arg(2), 0)), float(num(c.vm, c.arg(3), 0))};
        const float span = std::hypot(p1.x - p0.x, p1.y - p0.y) + std::hypot(p2.x - p1.x, p2.y - p1.y);
        const int n = std::clamp(int(std::ceil(span / 4.0f)), 2, 64);
        for (int i = 1; i <= n; ++i) {
            const float t = float(i) / float(n), u = 1.0f - t;
            line_to(*d, {u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x, u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y});
        }
        return {};
    });
    vm.define_method(mc, "beginFill", [drawing_of, close_fill](NativeCall& c) -> Value {
        Drawing* d = drawing_of(c);
        if (!d) {
            return {};
        }
        close_fill(*d);
        if (c.arg(0).is_null_or_undefined()) {
            return {}; // no colour: no fill
        }
        d->filling = true;
        d->fill.color = rgb_alpha(num(c.vm, c.arg(0), 0), num(c.vm, c.arg(1), 100) / 100.0);
        d->fill.contours = {{d->pen}};
        d->contour_start = d->pen;
        return {};
    });
    vm.define_method(mc, "endFill", [drawing_of, close_fill](NativeCall& c) -> Value {
        if (Drawing* d = drawing_of(c)) {
            close_fill(*d);
        }
        return {};
    });
    // Gradient and bitmap fills and gradient lines: no movie of the game uses them.
    for (const char* name : {"beginGradientFill", "beginBitmapFill", "lineGradientStyle"}) {
        vm.define_method(mc, name, [name](NativeCall& c) -> Value {
            c.vm.error(std::format("MovieClip.{} is not supported", name));
            return {};
        });
    }
    accessor(vm, mc, "hitArea",
             [](NativeCall& c) -> Value {
                 MovieClip* clip = clip_of(c.self);
                 return clip && clip->hit_area && clip->hit_area->object ? Value::object(clip->hit_area->object) : Value{};
             },
             [](NativeCall& c) -> Value {
                 if (MovieClip* clip = clip_of(c.self)) {
                     clip->hit_area = display_of(c.arg(0));
                 }
                 return {};
             });
    accessor(vm, mc, "filters", filters_getter, filters_setter);
    accessor(vm, mc, "blendMode", blend_getter, blend_setter);
    accessor(vm, mc, "cacheAsBitmap",
             [](NativeCall& c) -> Value {
                 DisplayObject* d = display_of(c.self);
                 return Value::boolean(d && d->cache_as_bitmap);
             },
             [](NativeCall& c) -> Value {
                 if (DisplayObject* d = display_of(c.self)) {
                     d->cache_as_bitmap = c.vm.to_boolean(c.arg(0));
                 }
                 return {};
             });
    accessor(vm, mc, "scrollRect",
             [geom](NativeCall& c) -> Value {
                 DisplayObject* d = display_of(c.self);
                 if (!d || !d->has_scroll_rect) {
                     return {};
                 }
                 return make_rect(c.vm, geom, d->scroll_rect[0], d->scroll_rect[1], d->scroll_rect[2], d->scroll_rect[3]);
             },
             [](NativeCall& c) -> Value {
                 DisplayObject* d = display_of(c.self);
                 if (!d) {
                     return {};
                 }
                 const Value r = c.arg(0);
                 d->has_scroll_rect = r.is_object();
                 if (r.is_object()) {
                     d->scroll_rect[0] = float(num(c.vm, c.vm.get(r, "x"), 0));
                     d->scroll_rect[1] = float(num(c.vm, c.vm.get(r, "y"), 0));
                     d->scroll_rect[2] = float(num(c.vm, c.vm.get(r, "width"), 0));
                     d->scroll_rect[3] = float(num(c.vm, c.vm.get(r, "height"), 0));
                 }
                 return {};
             });
    accessor(vm, mc, "scale9Grid",
             [geom](NativeCall& c) -> Value {
                 MovieClip* clip = clip_of(c.self);
                 if (!clip) {
                     return {};
                 }
                 const osu::Rect* r = clip->has_scale9 ? &clip->scale9_rect
                                      : clip->scale9() != osu::k_none ? &clip->movie->data().rects[clip->scale9()]
                                                                      : nullptr;
                 return r ? make_rect(c.vm, geom, r->x0, r->y0, r->x1 - r->x0, r->y1 - r->y0) : Value{};
             },
             [](NativeCall& c) -> Value {
                 MovieClip* clip = clip_of(c.self);
                 if (!clip) {
                     return {};
                 }
                 const Value r = c.arg(0);
                 clip->has_scale9 = r.is_object();
                 if (r.is_object()) {
                     const float x = float(num(c.vm, c.vm.get(r, "x"), 0)), y = float(num(c.vm, c.vm.get(r, "y"), 0));
                     clip->scale9_rect = {x, y, x + float(num(c.vm, c.vm.get(r, "width"), 0)),
                                          y + float(num(c.vm, c.vm.get(r, "height"), 0))};
                 }
                 return {};
             });
    accessor(vm, mc, "transform",
             [geom](NativeCall& c) -> Value { return c.vm.construct(Value::object(geom.transform), {c.self}); },
             [](NativeCall& c) -> Value {
                 DisplayObject* d = display_of(c.self);
                 const Value t = c.arg(0);
                 if (d && t.is_object()) {
                     const Value m = c.vm.get(t, "matrix"), cx = c.vm.get(t, "colorTransform");
                     if (m.is_object()) {
                         d->set_matrix(read_matrix(c.vm, m));
                     }
                     if (cx.is_object()) {
                         d->cxform = read_color_transform(c.vm, cx);
                     }
                     d->script_transformed = true;
                 }
                 return {};
             });
    // GFx extensions on display objects.
    auto flag_accessor = [&vm](Object* o, const char* name, bool DisplayObject::*field) {
        accessor(vm, o, name,
                 [field](NativeCall& c) -> Value {
                     DisplayObject* d = display_of(c.self);
                     return Value::boolean(d && d->*field);
                 },
                 [field](NativeCall& c) -> Value {
                     if (DisplayObject* d = display_of(c.self)) {
                         d->*field = c.vm.to_boolean(c.arg(0));
                     }
                     return {};
                 });
    };
    flag_accessor(mc, "topmostLevel", &DisplayObject::topmost);
    flag_accessor(mc, "hitTestDisable", &DisplayObject::hit_test_disable);

    // ---- Button --------------------------------------------------------------------------------
    Object* button_ctor = make_class(vm, g, "Button", [](NativeCall&) -> Value { return {}; });
    Object* bp = proto_of(vm, button_ctor);
    p.m_button_proto = bp;
    vm.define(bp, "enabled", Value::boolean(true));
    vm.define(bp, "useHandCursor", Value::boolean(true));
    vm.define_method(bp, "getDepth", [](NativeCall& c) -> Value {
        DisplayObject* d = display_of(c.self);
        return d ? Value::number(d->depth) : Value{};
    });
    accessor(vm, bp, "filters", filters_getter, filters_setter);
    accessor(vm, bp, "blendMode", blend_getter, blend_setter);
    flag_accessor(bp, "hitTestDisable", &DisplayObject::hit_test_disable);

    // ---- TextField -----------------------------------------------------------------------------
    Object* tf_ctor = make_class(vm, g, "TextField", [](NativeCall&) -> Value { return {}; });
    Object* tf = proto_of(vm, tf_ctor);
    p.m_textfield_proto = tf;
    vm.define_method(tf_ctor, "getFontList", [](NativeCall& c) -> Value { return Value::object(c.vm.make_array()); });
    auto text_prop = [&vm, tf](const char* name, std::function<Value(Vm&, TextFieldObject*)> get,
                               std::function<void(Vm&, TextFieldObject*, const Value&)> set) {
        accessor(vm, tf, name,
                 [get](NativeCall& c) -> Value {
                     TextFieldObject* t = text_of(c.self);
                     return t ? get(c.vm, t) : Value{};
                 },
                 set ? NativeFn([set](NativeCall& c) -> Value {
                     if (TextFieldObject* t = text_of(c.self)) {
                         set(c.vm, t, c.arg(0));
                     }
                     return {};
                 })
                     : NativeFn());
    };
    auto bool_text_prop = [&](const char* name, bool TextFieldObject::*field, bool relayout) {
        text_prop(
            name, [field](Vm&, TextFieldObject* t) { return Value::boolean(t->*field); },
            [field, relayout, &p](Vm& v, TextFieldObject* t, const Value& x) {
                t->*field = v.to_boolean(x);
                if (relayout) {
                    p.text_changed(t);
                }
            });
    };
    // `new TextField()` is on no stage, and still holds its text: the HUD's quest line passes "Started: " through one.
    accessor(vm, tf, "text",
             [](NativeCall& c) -> Value {
                 TextFieldObject* t = text_of(c.self);
                 return t ? Value::string(u32_to_utf8(t->plain_text())) : c.vm.get(c.self, "$offstage_text");
             },
             NativeFn([&p](NativeCall& c) -> Value {
                 if (TextFieldObject* t = text_of(c.self)) {
                     set_field_text(p, t, *c.vm.to_string(c.arg(0)), false);
                 } else {
                     c.vm.set(c.self, "$offstage_text", c.arg(0));
                 }
                 return {};
             }));
    text_prop("htmlText",
              [](Vm&, TextFieldObject* t) {
                  return Value::string(t->html ? t->html_text() : u32_to_utf8(t->plain_text()));
              },
              [&p](Vm& v, TextFieldObject* t, const Value& x) { set_field_text(p, t, *v.to_string(x), true); });
    bool_text_prop("html", &TextFieldObject::html, true);
    bool_text_prop("multiline", &TextFieldObject::multiline, true);
    bool_text_prop("wordWrap", &TextFieldObject::word_wrap, true);
    bool_text_prop("selectable", &TextFieldObject::selectable, false);
    bool_text_prop("border", &TextFieldObject::border, false);
    bool_text_prop("background", &TextFieldObject::background, false);
    bool_text_prop("embedFonts", &TextFieldObject::embed_fonts, true);
    bool_text_prop("password", &TextFieldObject::password, true);
    bool_text_prop("condenseWhite", &TextFieldObject::condense_white, false);
    bool_text_prop("noTranslate", &TextFieldObject::no_translate, false);
    text_prop("borderColor", [](Vm&, TextFieldObject* t) { return Value::number(rgba_to_rgb(t->border_color)); },
              [](Vm& v, TextFieldObject* t, const Value& x) { t->border_color = rgb_to_rgba(v.to_number(x)); });
    text_prop("backgroundColor",
              [](Vm&, TextFieldObject* t) { return Value::number(rgba_to_rgb(t->background_color)); },
              [](Vm& v, TextFieldObject* t, const Value& x) { t->background_color = rgb_to_rgba(v.to_number(x)); });
    text_prop("textColor", [](Vm&, TextFieldObject* t) { return Value::number(rgba_to_rgb(t->default_format.color)); },
              [&p](Vm& v, TextFieldObject* t, const Value& x) {
                  const uint32_t c2 = rgb_to_rgba(v.to_number(x));
                  t->default_format.color = c2;
                  if (t->translated_format) {
                      t->translated_format->color = c2;
                  }
                  for (auto& r : t->runs) {
                      r.format.color = c2;
                  }
                  p.text_changed(t);
              });
    text_prop("textWidth", [](Vm&, TextFieldObject* t) { return Value::number(t->text_width()); }, nullptr);
    // Whole pixels: measured in the game, a two-line description laid out 56.9 high reports 56.
    text_prop("textHeight", [](Vm&, TextFieldObject* t) { return Value::number(std::floor(t->text_height())); },
              nullptr);
    text_prop("length", [](Vm&, TextFieldObject* t) { return Value::number(double(t->plain_text().size())); }, nullptr);
    text_prop("maxChars",
              [](Vm&, TextFieldObject* t) { return t->max_chars > 0 ? Value::number(t->max_chars) : Value::null(); },
              [](Vm& v, TextFieldObject* t, const Value& x) {
                  t->max_chars = x.is_null_or_undefined() ? 0 : std::max(0, int(v.to_number(x)));
              });
    text_prop("autoSize", [](Vm&, TextFieldObject* t) { return Value::string(t->auto_size); },
              [&p](Vm& v, TextFieldObject* t, const Value& x) {
                  if (x.type == Type::boolean) {
                      t->auto_size = x.b ? "left" : "none";
                  } else {
                      const auto s = *v.to_string(x);
                      t->auto_size = (s == "left" || s == "right" || s == "center") ? s : "none";
                  }
                  p.text_changed(t);
              });
    text_prop("textAutoSize", [](Vm&, TextFieldObject* t) { return Value::string(t->text_auto_size); },
              [&p](Vm& v, TextFieldObject* t, const Value& x) {
                  const auto s = *v.to_string(x);
                  t->text_auto_size = (s == "shrink" || s == "fit") ? s : "none";
                  p.text_changed(t);
              });
    // GFx: the field's height follows its text, the named edge staying put. The Book movie's reference field
    // ("top") measures 2837.1 high in the game for Fall from Glory.
    text_prop("verticalAutoSize", [](Vm&, TextFieldObject* t) { return Value::string(t->vertical_auto_size); },
              [&p](Vm& v, TextFieldObject* t, const Value& x) {
                  const auto s = *v.to_string(x);
                  t->vertical_auto_size = (s == "top" || s == "center" || s == "bottom") ? s : "none";
                  p.text_changed(t);
              });
    text_prop("verticalAlign", [](Vm&, TextFieldObject* t) { return Value::string(t->vertical_align); },
              [&p](Vm& v, TextFieldObject* t, const Value& x) {
                  const auto s = *v.to_string(x);
                  t->vertical_align = (s == "top" || s == "center" || s == "bottom") ? s : "none";
                  p.text_changed(t);
              });
    text_prop("type", [](Vm&, TextFieldObject* t) { return Value::string(t->editable ? "input" : "dynamic"); },
              [](Vm& v, TextFieldObject* t, const Value& x) { t->editable = *v.to_string(x) == "input"; });
    text_prop("variable",
              [](Vm&, TextFieldObject* t) { return t->variable.empty() ? Value::null() : Value::string(t->variable); },
              [](Vm& v, TextFieldObject* t, const Value& x) {
                  t->variable = x.is_null_or_undefined() ? std::string() : *v.to_string(x);
              });
    text_prop("scroll", [](Vm&, TextFieldObject* t) { return Value::number(t->scroll); },
              [](Vm& v, TextFieldObject* t, const Value& x) {
                  const double d = v.to_number(x);
                  if (!std::isnan(d)) {
                      t->scroll = std::clamp(int(d), 1, max_scroll(t));
                  }
              });
    text_prop("maxscroll", [](Vm&, TextFieldObject* t) { return Value::number(max_scroll(t)); }, nullptr);
    text_prop("bottomScroll", [](Vm&, TextFieldObject* t) { return Value::number(bottom_scroll(t)); }, nullptr);
    text_prop("hscroll", [](Vm&, TextFieldObject* t) { return Value::number(t->hscroll); },
              [](Vm& v, TextFieldObject* t, const Value& x) { t->hscroll = std::max(0, int(v.to_number(x))); });
    text_prop("maxhscroll", [](Vm&, TextFieldObject* t) {
        return Value::number(std::max(0.0, double(t->text_width()) - (t->width - 2 * k_text_gutter)));
    }, nullptr);
    text_prop("numLines", [](Vm&, TextFieldObject* t) { return Value::number(double(t->laid_out().lines.size())); }, nullptr);
    text_prop("caretIndex", [](Vm&, TextFieldObject* t) { return Value::number(t->caret); },
              [](Vm& v, TextFieldObject* t, const Value& x) {
                  t->caret = std::clamp(int(v.to_number(x)), 0, int(t->plain_text().size()));
              });
    accessor(vm, tf, "filters", filters_getter, filters_setter);
    accessor(vm, tf, "blendMode", blend_getter, blend_setter);
    flag_accessor(tf, "hitTestDisable", &DisplayObject::hit_test_disable);
    flag_accessor(tf, "topmostLevel", &DisplayObject::topmost);

    Object* text_format = make_class(vm, g, "TextFormat", [](NativeCall& c) -> Value {
        const char* params[] = {"font", "size", "color", "bold", "italic", "underline", "url", "target", "align",
                                "leftMargin", "rightMargin", "indent", "leading"};
        for (const char* n2 : k_format_props) {
            c.vm.set(c.self, n2, Value::null());
        }
        for (size_t i = 0; i < std::size(params); ++i) {
            if (i < c.args.size() && !c.args[i].is_undefined()) {
                c.vm.set(c.self, params[i], c.args[i]);
            }
        }
        return {};
    });
    vm.define_method(proto_of(vm, text_format), "getTextExtent", [&p](NativeCall& c) -> Value {
        uint32_t mask;
        const TextFormat f = read_format(c.vm, c.self, mask);
        const auto text = utf8_to_u32(*c.vm.to_string(c.arg(0)));
        const FontRef font = p.library.logical_font(f.font, f.bold, f.italic);
        double w = 0;
        for (char32_t ch : text) {
            if (font) {
                const auto& m = font.movie->data();
                const auto& fd = m.fonts[font.font];
                for (uint32_t i = 0; i < fd.glyph_count; ++i) {
                    if (m.glyphs[fd.first_glyph + i].code == ch) {
                        w += m.glyphs[fd.first_glyph + i].advance * ((mask & fmt_size) ? f.size : 12);
                        break;
                    }
                }
            }
        }
        Object* o = c.vm.make_object();
        c.vm.set(Value::object(o), "width", Value::number(w));
        c.vm.set(Value::object(o), "textFieldWidth", Value::number(w + 2 * k_text_gutter));
        return Value::object(o);
    });

    auto format_object = [text_format](Vm& v, const TextFormat& f, uint32_t same) {
        const Value o = v.construct(Value::object(text_format), {});
        write_format(v, o, f, same);
        return o;
    };
    vm.define_method(tf, "getTextFormat", [format_object](NativeCall& c) -> Value {
        TextFieldObject* t = text_of(c.self);
        if (!t) {
            return {};
        }
        const size_t len = t->plain_text().size();
        size_t b = 0, e = len;
        if (!c.arg(0).is_undefined()) {
            b = size_t(std::clamp(num(c.vm, c.arg(0), 0), 0.0, double(len)));
            e = c.arg(1).is_undefined() ? b + 1 : size_t(std::clamp(num(c.vm, c.arg(1), 0), double(b), double(len)));
        }
        // Fields every character in the range shares; the others read back as null.
        const TextFormat* first = nullptr;
        uint32_t same = 0xFFFFFFFFu;
        size_t at = 0;
        for (const auto& r : t->runs) {
            const size_t r0 = at, r1 = at + r.text.size();
            at = r1;
            if (r1 <= b || r0 >= std::max(e, b + 1)) {
                continue;
            }
            if (!first) {
                first = &r.format;
            } else {
                same &= same_fields(*first, r.format);
            }
        }
        return format_object(c.vm, first ? *first : t->default_format, same);
    });
    vm.define_method(tf, "getNewTextFormat", [format_object](NativeCall& c) -> Value {
        TextFieldObject* t = text_of(c.self);
        return t ? format_object(c.vm, t->default_format, 0xFFFFFFFFu) : Value{};
    });
    vm.define_method(tf, "setNewTextFormat", [](NativeCall& c) -> Value {
        if (TextFieldObject* t = text_of(c.self)) {
            uint32_t mask;
            const TextFormat f = read_format(c.vm, c.arg(0), mask);
            merge_format(t->default_format, f, mask);
            if (t->translated_format) {
                merge_format(*t->translated_format, f, mask);
            }
        }
        return {};
    });
    vm.define_method(tf, "setTextFormat", [&p](NativeCall& c) -> Value {
        TextFieldObject* t = text_of(c.self);
        if (!t || c.args.empty()) {
            return {};
        }
        // setTextFormat(format), (index, format) or (begin, end, format).
        const Value fmt = c.args.back();
        uint32_t mask;
        const TextFormat f = read_format(c.vm, fmt, mask);
        const size_t len = t->plain_text().size();
        size_t b = 0, e = len;
        if (c.args.size() == 2) {
            b = size_t(std::clamp(num(c.vm, c.arg(0), 0), 0.0, double(len)));
            e = std::min(len, b + 1);
        } else if (c.args.size() >= 3) {
            b = size_t(std::clamp(num(c.vm, c.arg(0), 0), 0.0, double(len)));
            e = size_t(std::clamp(num(c.vm, c.arg(1), 0), double(b), double(len)));
        }
        if (t->runs.empty()) {
            return {};
        }
        auto [rb, re] = split_runs(t, b, e);
        for (size_t i = rb; i < re; ++i) {
            merge_format(t->runs[i].format, f, mask);
        }
        p.text_changed(t);
        return {};
    });
    vm.define_method(tf, "replaceText", [&p](NativeCall& c) -> Value {
        // GFx leaves the text alone for a negative begin or an end before the begin, and clamps the end to the
        // length: measured on a 449-character field, (-1, 5, "") and (5, 3, "") kept 449, (440, 100000, "") left
        // 440. The Book movie relies on it for a book's last page (CreateDisplayPage).
        if (TextFieldObject* t = text_of(c.self)) {
            const double b = num(c.vm, c.arg(0), 0), e = num(c.vm, c.arg(1), 0);
            if (!(b >= 0) || !(e >= b)) {
                return {};
            }
            replace_text(p, t, size_t(b), size_t(std::min(e, 1e9)), utf8_to_u32(*c.vm.to_string(c.arg(2))), true);
        }
        return {};
    });
    vm.define_method(tf, "replaceSel", [&p](NativeCall& c) -> Value {
        if (TextFieldObject* t = text_of(c.self)) {
            const int b = std::min(t->selection_begin, t->selection_end), e = std::max(t->selection_begin, t->selection_end);
            const auto text = utf8_to_u32(*c.vm.to_string(c.arg(0)));
            replace_text(p, t, size_t(std::max(0, b)), size_t(std::max(0, e)), text);
            t->caret = t->selection_begin = t->selection_end = b + int(text.size());
        }
        return {};
    });
    vm.define_method(tf, "appendText", [&p](NativeCall& c) -> Value {
        if (TextFieldObject* t = text_of(c.self)) {
            const size_t len = t->plain_text().size();
            replace_text(p, t, len, len, utf8_to_u32(*c.vm.to_string(c.arg(0))));
        }
        return {};
    });
    vm.define_method(tf, "appendHtml", [&p](NativeCall& c) -> Value {
        if (TextFieldObject* t = text_of(c.self)) {
            auto runs = parse_html(*c.vm.to_string(c.arg(0)), t->default_format, t->condense_white, t->multiline);
            t->runs.insert(t->runs.end(), runs.begin(), runs.end());
            p.text_changed(t);
        }
        return {};
    });
    vm.define_method(tf, "removeTextField", remove_self);
    vm.define_method(tf, "getDepth", [](NativeCall& c) -> Value {
        DisplayObject* d = display_of(c.self);
        return d ? Value::number(d->depth) : Value{};
    });
    vm.define_method(tf, "swapDepths", [](NativeCall& c) -> Value {
        DisplayObject* d = display_of(c.self);
        const double depth = c.vm.to_number(c.arg(0));
        if (d && d->parent && !std::isnan(depth)) {
            d->parent->swap_depths(d, int(depth));
        }
        return {};
    });
    vm.define_method(tf, "getLineMetrics", [](NativeCall& c) -> Value {
        TextFieldObject* t = text_of(c.self);
        if (!t) {
            return {};
        }
        const TextLayout& l = t->laid_out();
        const double i = num(c.vm, c.arg(0), -1);
        if (i < 0 || size_t(i) >= l.lines.size()) {
            return {};
        }
        const TextLine& line = l.lines[size_t(i)];
        Object* o = c.vm.make_object();
        const Value v = Value::object(o);
        // Measured in the game (GFx): x is 0 for a left-aligned line, without the 2-pixel gutter (the
        // Book movie's fields).
        c.vm.set(v, "x", Value::number(line.x));
        c.vm.set(v, "width", Value::number(line.width));
        c.vm.set(v, "height", Value::number(line.ascent + line.descent + line.leading));
        c.vm.set(v, "ascent", Value::number(line.ascent));
        c.vm.set(v, "descent", Value::number(line.descent));
        c.vm.set(v, "leading", Value::number(line.leading));
        return v;
    });
    // GFx's getLineIndexAtPoint(x, y): the line whose band (its top to the next line's, the text's own coordinates:
    // the first line from 0, the gutter not added) holds y; -1 above, below or for NaN; x is not tested. Measured on
    // a note: y -3 -1; y 0, 1.9, 2, 22, 26 and (500, 30) line 0 (35.5 tall); 100000 and NaN
    // -1. The Book movie cuts its pages with it at the tops getCharBoundaries gave (book.swf, CreateDisplayPage).
    vm.define_method(tf, "getLineIndexAtPoint", [](NativeCall& c) -> Value {
        TextFieldObject* t = text_of(c.self);
        const double y = c.args.size() >= 2 ? c.vm.to_number(c.arg(1)) : NAN;
        if (!t || std::isnan(y)) {
            return Value::number(-1);
        }
        // In whole twips, as GFx keeps its line positions: a page top computed in script from getCharBoundaries
        // lands on the line it came from, not in a float's width of gap between two lines.
        const TextLayout& l = t->laid_out();
        const auto twips = [](double px) { return std::llround(px * 20.0); };
        const auto top = [&](size_t i) { return twips(double(l.lines[i].y - l.lines[i].ascent - k_text_gutter)); };
        const long long at = twips(y);
        for (size_t i = 0; i < l.lines.size(); ++i) {
            const TextLine& line = l.lines[i];
            const long long end = i + 1 < l.lines.size()
                                      ? top(i + 1)
                                      : top(i) + twips(double(line.ascent + line.descent + line.leading));
            if (at >= top(i) && at < end) {
                return Value::number(double(i));
            }
        }
        return Value::number(-1);
    });
    vm.define_method(tf, "getCharBoundaries", [geom](NativeCall& c) -> Value {
        // GFx's TextField.getCharBoundaries(index) (AS2 extension): a flash.geom.Rectangle in the field's
        // space, x the character's pen position, width its advance, y the top of its line, and height the
        // line's height less the 2-pixel gutter. Measured in the game on a 24-pixel line of ascent 23.25 and
        // descent 5.2: the first character {x 2, y 2, width 7.95, height 26.45}; a second line's bottom
        // at 56.9; and on a note's line of ascent 21.95 and descent 13.55, {x 2, y 2,
        // width 15.85, height 33.5}. The Book movie paginates with its top and bottom
        // (book.swf, CalculatePagination). x leaves out the line's alignment: measured in the game (run
        // images9), the first character of right-aligned and centred lines at x 124
        // and 62 reports x 2, an image after 15 pixels of text x 17.
        TextFieldObject* t = text_of(c.self);
        const double i = num(c.vm, c.arg(0), -1);
        if (!t || i < 0) {
            return Value::null();
        }
        const TextLayout& l = t->laid_out();
        const size_t k = size_t(i);
        for (const TextLine& line : l.lines) {
            if (k < line.first_char || k >= line.first_char + line.char_count || k >= l.char_x.size()) {
                continue;
            }
            const float top = line.y - line.ascent;
            const float h = line.ascent + line.descent + line.leading - k_text_gutter;
            return c.vm.construct(Value::object(geom.rectangle),
                                  {Value::number(l.char_x[k] - line.align_shift), Value::number(top),
                                   Value::number(l.char_advance[k]), Value::number(h)});
        }
        return Value::null();
    });
    vm.define_method(tf, "getLineOffset", [](NativeCall& c) -> Value {
        TextFieldObject* t = text_of(c.self);
        const double i = num(c.vm, c.arg(0), -1);
        if (!t || i < 0 || size_t(i) >= t->laid_out().lines.size()) {
            return Value::number(-1);
        }
        return Value::number(t->laid_out().lines[size_t(i)].first_char);
    });
    vm.define_method(tf, "getLineLength", [](NativeCall& c) -> Value {
        TextFieldObject* t = text_of(c.self);
        const double i = num(c.vm, c.arg(0), -1);
        if (!t || i < 0 || size_t(i) >= t->laid_out().lines.size()) {
            return Value::number(-1);
        }
        return Value::number(t->laid_out().lines[size_t(i)].char_count);
    });
    vm.define_method(tf, "getLineText", [](NativeCall& c) -> Value {
        TextFieldObject* t = text_of(c.self);
        const double i = num(c.vm, c.arg(0), -1);
        if (!t || i < 0 || size_t(i) >= t->laid_out().lines.size()) {
            return {};
        }
        const TextLine& line = t->laid_out().lines[size_t(i)];
        const std::u32string all = t->plain_text();
        return Value::string(u32_to_utf8(std::u32string_view(all).substr(std::min<size_t>(line.first_char, all.size()), line.char_count)));
    });
    vm.define_method(tf, "addListener", [bc](NativeCall& c) -> Value {
        Object* o = c.self.object_or_null();
        if (!o) {
            return Value::boolean(false);
        }
        if (!o->props.find("_listeners")) {
            make_broadcaster(c.vm, o, bc);
        }
        return c.vm.call(bc.add, c.self, c.args);
    });
    vm.define_method(tf, "removeListener", [bc](NativeCall& c) -> Value { return c.vm.call(bc.remove, c.self, c.args); });

    // ---- Key, Mouse, Selection, Stage ----------------------------------------------------------
    Object* key = vm.make_object();
    vm.define(g, "Key", Value::object(key));
    make_broadcaster(vm, key, bc);
    const std::pair<const char*, int> key_codes[] = {
        {"BACKSPACE", 8}, {"CAPSLOCK", 20}, {"CONTROL", 17}, {"DELETEKEY", 46}, {"DOWN", 40}, {"END", 35},
        {"ENTER", 13},    {"ESCAPE", 27},   {"HOME", 36},    {"INSERT", 45},    {"LEFT", 37}, {"PGDN", 34},
        {"PGUP", 33},     {"RIGHT", 39},    {"SHIFT", 16},   {"SPACE", 32},     {"TAB", 9},   {"UP", 38},
        {"ALT", 18}};
    for (const auto& [name, code] : key_codes) {
        vm.define(key, name, Value::number(code), prop_dont_enum | prop_dont_delete | prop_read_only);
    }
    vm.define_method(key, "getCode", [&p](NativeCall&) -> Value { return Value::number(p.last_key()); });
    vm.define_method(key, "getAscii", [&p](NativeCall&) -> Value { return Value::number(p.last_ascii()); });
    vm.define_method(key, "isDown", [&p](NativeCall& c) -> Value {
        return Value::boolean(p.key_down(uint32_t(num(c.vm, c.arg(0), 0))));
    });
    vm.define_method(key, "isToggled", [](NativeCall&) -> Value { return Value::boolean(false); });
    vm.define_method(key, "isAccessible", [](NativeCall&) -> Value { return Value::boolean(false); });

    Object* mouse = vm.make_object();
    vm.define(g, "Mouse", Value::object(mouse));
    make_broadcaster(vm, mouse, bc);
    vm.define_method(mouse, "show", [&p](NativeCall&) -> Value {
        const bool was = p.cursor_shown;
        p.cursor_shown = true;
        return Value::number(was ? 1 : 0);
    });
    vm.define_method(mouse, "hide", [&p](NativeCall&) -> Value {
        const bool was = p.cursor_shown;
        p.cursor_shown = false;
        return Value::number(was ? 1 : 0);
    });
    // GFx: getTopMostEntity([testAll]) or (x, y[, testAll]); testAll false means mouse targets only.
    vm.define_method(mouse, "getTopMostEntity", [&p](NativeCall& c) -> Value {
        float x = p.mouse_x(), y = p.mouse_y();
        bool test_all = true;
        if (c.args.size() >= 2 && c.arg(0).is_number()) {
            x = float(c.vm.to_number(c.arg(0)));
            y = float(c.vm.to_number(c.arg(1)));
            if (c.args.size() >= 3) {
                test_all = c.vm.to_boolean(c.arg(2));
            }
        } else if (!c.args.empty() && c.arg(0).type == Type::boolean) {
            test_all = c.arg(0).b;
        }
        DisplayObject* d = p.topmost_entity(x, y, !test_all);
        return d && d->object ? Value::object(d->object) : Value{};
    });
    vm.define_method(mouse, "getPosition", [&p, geom](NativeCall& c) -> Value {
        return make_point(c.vm, geom, p.mouse_x(), p.mouse_y());
    });
    vm.define_method(mouse, "getButtonsState", [](NativeCall&) -> Value { return Value::number(0); });
    vm.define_method(mouse, "setCursorType", [](NativeCall&) -> Value { return {}; });
    vm.define(mouse, "LEFT", Value::number(1));
    vm.define(mouse, "RIGHT", Value::number(2));
    vm.define(mouse, "MIDDLE", Value::number(3));

    Object* selection = vm.make_object();
    vm.define(g, "Selection", Value::object(selection));
    make_broadcaster(vm, selection, bc);
    vm.define_method(selection, "getFocus", [&p](NativeCall&) -> Value {
        DisplayObject* f = p.focus();
        return f && !f->removed ? Value::string(f->target_path()) : Value::null();
    });
    vm.define_method(selection, "setFocus", [&p](NativeCall& c) -> Value {
        const Value t = c.arg(0);
        DisplayObject* d = display_of(t);
        if (!d && t.is_string()) {
            d = p.resolve_target(p.root.get(), *t.s);
        }
        p.set_focus(d);
        return Value::boolean(true);
    });
    auto focused_text = [&p]() -> TextFieldObject* {
        DisplayObject* f = p.focus();
        return f && f->kind == DisplayKind::text ? static_cast<TextFieldObject*>(f) : nullptr;
    };
    vm.define_method(selection, "getBeginIndex", [focused_text](NativeCall&) -> Value {
        TextFieldObject* t = focused_text();
        return Value::number(t ? std::min(t->selection_begin, t->selection_end) : -1);
    });
    vm.define_method(selection, "getEndIndex", [focused_text](NativeCall&) -> Value {
        TextFieldObject* t = focused_text();
        return Value::number(t ? std::max(t->selection_begin, t->selection_end) : -1);
    });
    vm.define_method(selection, "getCaretIndex", [focused_text](NativeCall&) -> Value {
        TextFieldObject* t = focused_text();
        return Value::number(t ? t->caret : -1);
    });
    vm.define_method(selection, "setSelection", [focused_text](NativeCall& c) -> Value {
        if (TextFieldObject* t = focused_text()) {
            const int len = int(t->plain_text().size());
            t->selection_begin = std::clamp(int(num(c.vm, c.arg(0), 0)), 0, len);
            t->selection_end = std::clamp(int(num(c.vm, c.arg(1), 0)), 0, len);
            t->caret = t->selection_end;
        }
        return {};
    });
    // GFx focus extensions; one keyboard and one mouse, so one focus group.
    vm.define_method(selection, "getControllerFocusGroup", [](NativeCall&) -> Value { return Value::number(0); });
    vm.define_method(selection, "setControllerFocusGroup", [](NativeCall&) -> Value { return Value::boolean(true); });
    vm.define_method(selection, "getControllerMaskByFocusGroup", [](NativeCall&) -> Value { return Value::number(1); });
    // getFocusBitmask(obj): a bit for each controller whose focus is obj; CLIK's TextInput.changeFocus hands its
    // text field the focus of each controller set here (racesex_menu.swf's name entry).
    vm.define_method(selection, "getFocusBitmask", [&p](NativeCall& c) -> Value {
        DisplayObject* d = display_of(c.arg(0));
        return Value::number(d && d == p.focus() ? 1 : 0);
    });
    vm.define_method(selection, "getFocusArray", [&p](NativeCall& c) -> Value {
        DisplayObject* f = p.focus();
        std::vector<Value> out;
        if (f && f->object) {
            out.push_back(Value::object(f->object));
        }
        return Value::object(c.vm.make_array(std::move(out)));
    });
    vm.define_method(selection, "captureFocus", [](NativeCall&) -> Value { return {}; });
    vm.define_method(selection, "moveFocus", [](NativeCall&) -> Value { return {}; });
    vm.define(selection, "numFocusGroups", Value::number(1));

    Object* stage = vm.make_object();
    vm.define(g, "Stage", Value::object(stage));
    make_broadcaster(vm, stage, bc);
    // Stage.width/height: the player's size under noScale, the movie's otherwise (Flash 8 Stage).
    accessor(vm, stage, "width", [&p](NativeCall&) -> Value {
        return Value::number(p.stage.scale_mode == "noScale" ? p.stage.viewport[0] : p.movie->data().header.stage_width);
    });
    accessor(vm, stage, "height", [&p](NativeCall&) -> Value {
        return Value::number(p.stage.scale_mode == "noScale" ? p.stage.viewport[1] : p.movie->data().header.stage_height);
    });
    accessor(vm, stage, "scaleMode", [&p](NativeCall&) -> Value { return Value::string(p.stage.scale_mode); },
             [&p](NativeCall& c) -> Value { p.stage.scale_mode = *c.vm.to_string(c.arg(0)); return {}; });
    accessor(vm, stage, "align", [&p](NativeCall&) -> Value { return Value::string(p.stage.align); },
             [&p](NativeCall& c) -> Value { p.stage.align = *c.vm.to_string(c.arg(0)); return {}; });
    vm.define(stage, "showMenu", Value::boolean(false));
    auto stage_rect = [geom](Vm& v, const float r[4]) { return make_rect(v, geom, r[0], r[1], r[2] - r[0], r[3] - r[1]); };
    accessor(vm, stage, "visibleRect", [&p, stage_rect](NativeCall& c) -> Value { return stage_rect(c.vm, p.stage.visible); });
    accessor(vm, stage, "safeRect", [&p, stage_rect](NativeCall& c) -> Value { return stage_rect(c.vm, p.stage.safe); });
    accessor(vm, stage, "originalRect", [&p, geom](NativeCall& c) -> Value {
        return make_rect(c.vm, geom, 0, 0, p.movie->data().header.stage_width, p.movie->data().header.stage_height);
    });

    // ---- System --------------------------------------------------------------------------------
    Object* system = vm.make_object();
    vm.define(g, "System", Value::object(system));
    Object* caps = vm.make_object();
    vm.define(system, "capabilities", Value::object(caps));
    vm.set(Value::object(caps), "os", Value::string("Windows"));
    vm.set(Value::object(caps), "playerType", Value::string("StandAlone"));
    vm.set(Value::object(caps), "language", Value::string("en"));
    vm.set(Value::object(caps), "hasIME", Value::boolean(false));
    // GFx's controller count (1 by default: one keyboard and mouse, as
    // numFocusGroups above): CLIK's UIComponent.focused setter loops over it to give each controller its focus, so
    // without it no CLIK component takes focus (the Race Sex Menu's name entry, racesex_menu.swf).
    vm.set(Value::object(caps), "numControllers", Value::number(1));
    vm.set(Value::object(caps), "screenResolutionX", Value::number(p.movie->data().header.stage_width));
    vm.set(Value::object(caps), "screenResolutionY", Value::number(p.movie->data().header.stage_height));
    Object* security = vm.make_object();
    vm.define(system, "security", Value::object(security));
    vm.define_method(security, "allowDomain", [](NativeCall&) -> Value { return {}; });
    vm.define_method(security, "allowInsecureDomain", [](NativeCall&) -> Value { return {}; });
    vm.define(system, "useCodepage", Value::boolean(false));
    vm.define_method(system, "setClipboard", [](NativeCall&) -> Value { return {}; });

    // ---- ExternalInterface ---------------------------------------------------------------------
    Object* ext = vm.make_object();
    vm.define(pkg_external, "ExternalInterface", Value::object(ext));
    vm.define(ext, "available", Value::boolean(true));
    vm.define_method(ext, "call", [&p](NativeCall& c) -> Value {
        if (!p.host || c.args.empty()) {
            return Value::null();
        }
        const std::string method = *c.vm.to_string(c.arg(0));
        const std::vector<Value> args(c.args.begin() + 1, c.args.end());
        return p.host->external_call(p, method, args);
    });
    vm.define_method(ext, "addCallback", [&p](NativeCall& c) -> Value {
        if (c.args.size() < 3) {
            return Value::boolean(false);
        }
        p.m_external_callbacks[*c.vm.to_string(c.arg(0))] = {c.arg(1), c.arg(2)};
        return Value::boolean(true);
    });

    // ---- MovieClipLoader -----------------------------------------------------------------------
    Object* loader = make_class(vm, g, "MovieClipLoader", [bc](NativeCall& c) -> Value {
        if (Object* o = c.self.object_or_null()) {
            make_broadcaster(c.vm, o, bc);
            // Flash 8: the loader is its own first listener, so handlers set on it fire too.
            c.vm.call(bc.add, c.self, {c.self});
        }
        return {};
    });
    {
        Object* lp = proto_of(vm, loader);
        vm.define_method(lp, "loadClip", [&p](NativeCall& c) -> Value {
            const std::string url = *c.vm.to_string(c.arg(0));
            MovieClip* target = clip_of(c.arg(1));
            if (!target && c.arg(1).is_string()) {
                DisplayObject* d = p.resolve_target(p.root.get(), *c.arg(1).s);
                target = d && d->kind == DisplayKind::clip ? static_cast<MovieClip*>(d) : nullptr;
            }
            if (!target) {
                return Value::boolean(false);
            }
            const Value self = c.self;
            Object* target_obj = target->object;
            p.pin(self.o);
            p.queue_load(
                target, url,
                [&p, self, target_obj](bool ok) {
                    const Value t = Value::object(target_obj);
                    if (ok) {
                        p.vm.call_method(self, "broadcastMessage", {Value::string("onLoadStart"), t});
                        p.vm.call_method(self, "broadcastMessage", {Value::string("onLoadProgress"), t, Value::number(1), Value::number(1)});
                        p.vm.call_method(self, "broadcastMessage", {Value::string("onLoadComplete"), t, Value::number(0)});
                    } else {
                        p.vm.call_method(self, "broadcastMessage",
                                         {Value::string("onLoadError"), t, Value::string("URLNotFound"), Value::number(0)});
                    }
                },
                [&p, self, target_obj] {
                    p.vm.call_method(self, "broadcastMessage", {Value::string("onLoadInit"), Value::object(target_obj)});
                });
            return Value::boolean(true);
        });
        vm.define_method(lp, "unloadClip", [](NativeCall& c) -> Value {
            if (MovieClip* clip = clip_of(c.arg(0))) {
                std::vector<int> depths;
                for (const auto& [d, ch] : clip->children) {
                    depths.push_back(d);
                }
                for (int d : depths) {
                    clip->remove_child(d);
                }
                clip->engine_image = ImageRegistry::k_none;
            }
            return Value::boolean(true);
        });
        vm.define_method(lp, "getProgress", [](NativeCall& c) -> Value {
            Object* o = c.vm.make_object();
            c.vm.set(Value::object(o), "bytesLoaded", Value::number(1));
            c.vm.set(Value::object(o), "bytesTotal", Value::number(1));
            return Value::object(o);
        });
    }

    // ---- Color (Flash 5) -----------------------------------------------------------------------
    Object* color = make_class(vm, g, "Color", [](NativeCall& c) -> Value {
        if (Object* o = c.self.object_or_null()) {
            c.vm.define(o, "__target", c.arg(0));
        }
        return {};
    });
    {
        Object* cp = proto_of(vm, color);
        auto target = [&p](NativeCall& c) -> DisplayObject* {
            const Value t = c.vm.get(c.self, "__target");
            DisplayObject* d = display_of(t);
            if (!d && t.is_string()) {
                d = p.resolve_target(p.root.get(), *t.s);
            }
            return d;
        };
        vm.define_method(cp, "setRGB", [target](NativeCall& c) -> Value {
            if (DisplayObject* d = target(c)) {
                const uint32_t v = uint32_t(int64_t(num(c.vm, c.arg(0), 0)));
                for (int i = 0; i < 3; ++i) {
                    d->cxform.mul[i] = 0;
                }
                d->cxform.add[0] = float((v >> 16) & 255) / 255.0f;
                d->cxform.add[1] = float((v >> 8) & 255) / 255.0f;
                d->cxform.add[2] = float(v & 255) / 255.0f;
                d->script_transformed = true;
            }
            return {};
        });
        vm.define_method(cp, "getRGB", [target](NativeCall& c) -> Value {
            DisplayObject* d = target(c);
            if (!d) {
                return {};
            }
            const auto ch = [&](int i) { return uint32_t(std::lround(d->cxform.add[i] * 255.0f)) & 255; };
            return Value::number(double(ch(0) << 16 | ch(1) << 8 | ch(2)));
        });
        vm.define_method(cp, "setTransform", [target](NativeCall& c) -> Value {
            DisplayObject* d = target(c);
            const Value t = c.arg(0);
            if (!d || !t.is_object()) {
                return {};
            }
            const char* mul[4] = {"ra", "ga", "ba", "aa"};
            const char* add[4] = {"rb", "gb", "bb", "ab"};
            for (int i = 0; i < 4; ++i) {
                const Value m = c.vm.get(t, mul[i]), a = c.vm.get(t, add[i]);
                if (!m.is_undefined()) {
                    d->cxform.mul[i] = float(c.vm.to_number(m) / 100.0);
                }
                if (!a.is_undefined()) {
                    d->cxform.add[i] = float(c.vm.to_number(a) / 255.0);
                }
            }
            d->script_transformed = true;
            return {};
        });
        vm.define_method(cp, "getTransform", [target](NativeCall& c) -> Value {
            DisplayObject* d = target(c);
            if (!d) {
                return {};
            }
            Object* o = c.vm.make_object();
            const char* mul[4] = {"ra", "ga", "ba", "aa"};
            const char* add[4] = {"rb", "gb", "bb", "ab"};
            for (int i = 0; i < 4; ++i) {
                c.vm.set(Value::object(o), mul[i], Value::number(d->cxform.mul[i] * 100.0));
                c.vm.set(Value::object(o), add[i], Value::number(d->cxform.add[i] * 255.0));
            }
            return Value::object(o);
        });
    }

    // ---- Sound (UI sounds go through the engine; the class only has to exist) ------------------
    {
        Object* sound = make_class(vm, g, "Sound", [](NativeCall&) -> Value { return {}; });
        Object* sp = proto_of(vm, sound);
        for (const char* name : {"attachSound", "start", "stop", "loadSound", "setVolume", "setPan", "setTransform"}) {
            vm.define_method(sp, name, [](NativeCall&) -> Value { return {}; });
        }
        vm.define_method(sp, "getVolume", [](NativeCall&) -> Value { return Value::number(100); });
        vm.define_method(sp, "getPan", [](NativeCall&) -> Value { return Value::number(0); });
    }

    // ---- Date ----------------------------------------------------------------------------------
    {
        Object* date = make_class(vm, g, "Date", [](NativeCall& c) -> Value {
            double ms;
            if (c.args.empty()) {
                ms = double(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());
            } else if (c.args.size() == 1) {
                ms = c.vm.to_number(c.arg(0));
            } else {
                std::tm tm{};
                tm.tm_year = int(num(c.vm, c.arg(0), 1970)) - 1900;
                tm.tm_mon = int(num(c.vm, c.arg(1), 0));
                tm.tm_mday = int(num(c.vm, c.arg(2), 1));
                tm.tm_hour = int(num(c.vm, c.arg(3), 0));
                tm.tm_min = int(num(c.vm, c.arg(4), 0));
                tm.tm_sec = int(num(c.vm, c.arg(5), 0));
                tm.tm_isdst = -1;
                ms = double(std::mktime(&tm)) * 1000.0 + num(c.vm, c.arg(6), 0);
            }
            if (Object* o = c.self.object_or_null()) {
                o->primitive = Value::number(ms);
            }
            return {};
        });
        Object* dp = proto_of(vm, date);
        auto ms_of = [](NativeCall& c) { return c.self.is_object() ? c.vm.to_number(c.self.o->primitive) : 0.0; };
        vm.define_method(dp, "getTime", [ms_of](NativeCall& c) -> Value { return Value::number(ms_of(c)); });
        vm.define_method(dp, "valueOf", [ms_of](NativeCall& c) -> Value { return Value::number(ms_of(c)); });
        vm.define_method(dp, "setTime", [](NativeCall& c) -> Value {
            if (Object* o = c.self.object_or_null()) {
                o->primitive = Value::number(c.vm.to_number(c.arg(0)));
            }
            return {};
        });
        auto local = [ms_of](NativeCall& c) {
            const std::time_t t = std::time_t(ms_of(c) / 1000.0);
            std::tm tm{};
            localtime_s(&tm, &t);
            return tm;
        };
        vm.define_method(dp, "getFullYear", [local](NativeCall& c) -> Value { return Value::number(local(c).tm_year + 1900); });
        vm.define_method(dp, "getYear", [local](NativeCall& c) -> Value { return Value::number(local(c).tm_year); });
        vm.define_method(dp, "getMonth", [local](NativeCall& c) -> Value { return Value::number(local(c).tm_mon); });
        vm.define_method(dp, "getDate", [local](NativeCall& c) -> Value { return Value::number(local(c).tm_mday); });
        vm.define_method(dp, "getDay", [local](NativeCall& c) -> Value { return Value::number(local(c).tm_wday); });
        vm.define_method(dp, "getHours", [local](NativeCall& c) -> Value { return Value::number(local(c).tm_hour); });
        vm.define_method(dp, "getMinutes", [local](NativeCall& c) -> Value { return Value::number(local(c).tm_min); });
        vm.define_method(dp, "getSeconds", [local](NativeCall& c) -> Value { return Value::number(local(c).tm_sec); });
        vm.define_method(dp, "getMilliseconds", [ms_of](NativeCall& c) -> Value {
            return Value::number(std::fmod(ms_of(c), 1000.0));
        });
        vm.define_method(date, "UTC", [](NativeCall& c) -> Value {
            std::tm tm{};
            tm.tm_year = int(num(c.vm, c.arg(0), 1970)) - 1900;
            tm.tm_mon = int(num(c.vm, c.arg(1), 0));
            tm.tm_mday = int(num(c.vm, c.arg(2), 1));
            tm.tm_hour = int(num(c.vm, c.arg(3), 0));
            tm.tm_min = int(num(c.vm, c.arg(4), 0));
            tm.tm_sec = int(num(c.vm, c.arg(5), 0));
            return Value::number(double(_mkgmtime(&tm)) * 1000.0 + num(c.vm, c.arg(6), 0));
        });
    }

    // ---- intervals and timeouts ----------------------------------------------------------------
    auto interval = [&p](NativeCall& c, bool once) -> Value {
        // setInterval(function, ms, args...) or setInterval(object, "method", ms, args...).
        if (c.args.size() < 2) {
            return {};
        }
        const Value a0 = c.arg(0);
        if (a0.is_object() && a0.o->is_function()) {
            const std::vector<Value> rest(c.args.begin() + 2, c.args.end());
            return Value::number(p.add_interval(a0, Value{}, c.vm.to_number(c.arg(1)), rest, once));
        }
        if (c.args.size() < 3) {
            return {};
        }
        const std::vector<Value> rest(c.args.begin() + 3, c.args.end());
        return Value::number(p.add_interval(a0, Value::string(c.vm.to_string(c.arg(1))), c.vm.to_number(c.arg(2)), rest, once));
    };
    vm.define_method(g, "setInterval", [interval](NativeCall& c) -> Value { return interval(c, false); });
    vm.define_method(g, "setTimeout", [interval](NativeCall& c) -> Value { return interval(c, true); });
    auto clear = [&p](NativeCall& c) -> Value {
        const double id = c.vm.to_number(c.arg(0));
        if (!std::isnan(id)) {
            p.clear_interval(uint32_t(id));
        }
        return {};
    };
    vm.define_method(g, "clearInterval", clear);
    vm.define_method(g, "clearTimeout", clear);
    vm.define_method(g, "updateAfterEvent", [](NativeCall&) -> Value { return {}; });
    vm.define_method(g, "targetPath", [](NativeCall& c) -> Value {
        DisplayObject* d = display_of(c.arg(0));
        return d ? Value::string(d->target_path()) : Value{};
    });
}

} // namespace spud::ui::player
