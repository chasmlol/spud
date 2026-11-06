#include "formats/osu.hpp"

#include "core/error.hpp"

#include <cstring>

namespace spud::osu {

namespace {

using container::fourcc;

constexpr auto k_head = fourcc("HEAD");
constexpr auto k_strs = fourcc("STRS");

template <typename T>
void put(container::Writer& w, const char (&id)[5], const std::vector<T>& v) {
    w.add_array(fourcc(id), v);
}

template <typename T>
void get(const container::Reader& r, const char (&id)[5], std::vector<T>& v) {
    if (r.find(fourcc(id))) {
        v = r.read_array<T>(fourcc(id));
    }
}

std::vector<char> read_chars(const container::Reader& r) {
    std::vector<char> out;
    if (auto s = r.find(k_strs)) {
        out.resize(s->size());
        std::memcpy(out.data(), s->data(), s->size());
    }
    return out;
}

} // namespace

std::string_view Movie::string(uint32_t offset) const {
    if (offset == k_none) {
        return {};
    }
    if (offset >= strings.size()) {
        throw ParseError("osu: string offset out of range");
    }
    return std::string_view(strings.data() + offset);
}

std::vector<std::byte> write(const Movie& m) {
    container::Writer w(k_kind_movie, k_version);
    w.add_struct(k_head, m.header);
    put(w, "STRS", m.strings);
    put(w, "CHAR", m.characters);
    put(w, "SHAP", m.shapes);
    put(w, "MESH", m.meshes);
    put(w, "VERT", m.vertices);
    put(w, "INDX", m.indices);
    put(w, "GRAD", m.gradients);
    put(w, "GSTP", m.gradient_stops);
    put(w, "BMAP", m.bitmaps);
    put(w, "BDAT", m.bitmap_data);
    put(w, "SPRT", m.sprites);
    put(w, "FRAM", m.frames);
    put(w, "LABL", m.labels);
    put(w, "FACT", m.frame_actions);
    put(w, "INIT", m.init_actions);
    put(w, "PLCE", m.commands);
    put(w, "FILT", m.filters);
    put(w, "FVAL", m.filter_values);
    put(w, "EVNT", m.events);
    put(w, "RECT", m.rects);
    put(w, "TEXT", m.texts);
    put(w, "FONT", m.fonts);
    put(w, "GLYF", m.glyphs);
    put(w, "KERN", m.kernings);
    put(w, "BUTN", m.buttons);
    put(w, "BREC", m.button_records);
    put(w, "BACT", m.button_actions);
    put(w, "EXPT", m.exports);
    put(w, "IMPT", m.imports);
    put(w, "FUNC", m.functions);
    put(w, "PARM", m.params);
    put(w, "CODE", m.instructions);
    put(w, "PVAL", m.values);
    return w.finish();
}

Movie read(std::span<const std::byte> file) {
    container::Reader r(file, k_kind_movie);
    if (r.version() != k_version) {
        throw ParseError("osu: unsupported movie version " + std::to_string(r.version()));
    }
    Movie m;
    m.header = r.read_struct<Header>(k_head);
    m.strings = read_chars(r);
    get(r, "CHAR", m.characters);
    get(r, "SHAP", m.shapes);
    get(r, "MESH", m.meshes);
    get(r, "VERT", m.vertices);
    get(r, "INDX", m.indices);
    get(r, "GRAD", m.gradients);
    get(r, "GSTP", m.gradient_stops);
    get(r, "BMAP", m.bitmaps);
    get(r, "BDAT", m.bitmap_data);
    get(r, "SPRT", m.sprites);
    get(r, "FRAM", m.frames);
    get(r, "LABL", m.labels);
    get(r, "FACT", m.frame_actions);
    get(r, "INIT", m.init_actions);
    get(r, "PLCE", m.commands);
    get(r, "FILT", m.filters);
    get(r, "FVAL", m.filter_values);
    get(r, "EVNT", m.events);
    get(r, "RECT", m.rects);
    get(r, "TEXT", m.texts);
    get(r, "FONT", m.fonts);
    get(r, "GLYF", m.glyphs);
    get(r, "KERN", m.kernings);
    get(r, "BUTN", m.buttons);
    get(r, "BREC", m.button_records);
    get(r, "BACT", m.button_actions);
    get(r, "EXPT", m.exports);
    get(r, "IMPT", m.imports);
    get(r, "FUNC", m.functions);
    get(r, "PARM", m.params);
    get(r, "CODE", m.instructions);
    get(r, "PVAL", m.values);
    if (m.header.root_sprite >= m.sprites.size()) {
        throw ParseError("osu: root sprite out of range");
    }
    return m;
}

std::vector<std::byte> write(const Strings& t) {
    container::Writer w(k_kind_strings, k_version);
    put(w, "STRS", t.strings);
    put(w, "TRAN", t.entries);
    return w.finish();
}

Strings read_strings(std::span<const std::byte> file) {
    container::Reader r(file, k_kind_strings);
    Strings t;
    t.strings = read_chars(r);
    get(r, "TRAN", t.entries);
    return t;
}

std::vector<std::byte> write(const FontMap& f) {
    container::Writer w(k_kind_fonts, k_version);
    put(w, "STRS", f.strings);
    put(w, "FLIB", f.libraries);
    put(w, "FNAM", f.names);
    return w.finish();
}

std::vector<std::byte> write(const Controls& c) {
    container::Writer w(k_kind_controls, k_version);
    put(w, "STRS", c.strings);
    put(w, "CKEY", c.keys);
    put(w, "CCTX", c.contexts);
    put(w, "CBND", c.bindings);
    return w.finish();
}

Controls read_controls(std::span<const std::byte> file) {
    container::Reader r(file, k_kind_controls);
    Controls c;
    c.strings = read_chars(r);
    get(r, "CKEY", c.keys);
    get(r, "CCTX", c.contexts);
    get(r, "CBND", c.bindings);
    return c;
}

FontMap read_font_map(std::span<const std::byte> file) {
    container::Reader r(file, k_kind_fonts);
    FontMap f;
    f.strings = read_chars(r);
    get(r, "FLIB", f.libraries);
    get(r, "FNAM", f.names);
    return f;
}

} // namespace spud::osu
