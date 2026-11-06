#include "importer/legacy/swf.hpp"

#include "core/error.hpp"

#include <zlib.h>

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG // the interface images books name (importer/convert/ui_images.cpp)
#define STBI_NO_STDIO
#include <stb_image.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <cstring>
#include <format>

namespace spud::legacy::swf {

namespace {

std::vector<std::byte> inflate_all(std::span<const std::byte> in, size_t expected) {
    std::vector<std::byte> out(expected);
    z_stream z{};
    if (inflateInit(&z) != Z_OK) {
        throw ParseError("swf: inflateInit failed");
    }
    z.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(in.data()));
    z.avail_in = static_cast<uInt>(in.size());
    z.next_out = reinterpret_cast<Bytef*>(out.data());
    z.avail_out = static_cast<uInt>(out.size());
    const int rc = inflate(&z, Z_FINISH);
    const size_t produced = out.size() - z.avail_out;
    inflateEnd(&z);
    if (rc != Z_STREAM_END && rc != Z_OK && rc != Z_BUF_ERROR) {
        throw ParseError(std::format("swf: zlib error {}", rc));
    }
    out.resize(produced);
    return out;
}

uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
}

FillStyle read_fill_style(BitReader& r, int version) {
    FillStyle f;
    f.type = r.u8();
    if (f.type == 0x00) {
        f.color = version >= 3 ? read_rgba(r) : read_rgb(r);
    } else if (f.type == 0x10 || f.type == 0x12 || f.type == 0x13) {
        f.matrix = read_matrix(r);
        const uint8_t bits = r.u8();
        f.spread = (bits >> 6) & 3;
        f.interpolation = (bits >> 4) & 3;
        const unsigned count = bits & 0x0F;
        for (unsigned i = 0; i < count; ++i) {
            GradientRecord g;
            g.ratio = r.u8();
            g.color = version >= 3 ? read_rgba(r) : read_rgb(r);
            f.gradient.push_back(g);
        }
        if (f.type == 0x13) {
            f.focal = r.fixed8();
        }
    } else if (f.type >= 0x40 && f.type <= 0x43) {
        f.bitmap = r.u16();
        f.matrix = read_matrix(r);
    } else {
        throw ParseError(std::format("swf: unknown fill style type 0x{:02x}", f.type));
    }
    return f;
}

void read_styles(BitReader& r, int version, Shape& s) {
    unsigned n = r.u8();
    if (n == 0xFF && version >= 2) {
        n = r.u16();
    }
    s.group_fill_start.push_back(static_cast<uint32_t>(s.fills.size()));
    for (unsigned i = 0; i < n; ++i) {
        s.fills.push_back(read_fill_style(r, version));
    }
    n = r.u8();
    if (n == 0xFF) {
        n = r.u16();
    }
    s.group_line_start.push_back(static_cast<uint32_t>(s.lines.size()));
    for (unsigned i = 0; i < n; ++i) {
        LineStyle l;
        l.width = r.u16();
        if (version == 4) {
            const unsigned flags = r.u16();
            l.start_cap = (flags >> 14) & 3;
            l.join = (flags >> 12) & 3;
            l.has_fill = (flags >> 11) & 1;
            l.no_close = (flags >> 2) & 1;
            l.end_cap = flags & 3;
            if (l.join == 2) {
                l.miter_limit = r.fixed8();
            }
            if (l.has_fill) {
                l.fill = read_fill_style(r, version);
                l.color = l.fill.type == 0 ? l.fill.color : 0xFFFFFFFFu;
            } else {
                l.color = read_rgba(r);
            }
        } else {
            l.color = version >= 3 ? read_rgba(r) : read_rgb(r);
        }
        s.lines.push_back(l);
    }
}

Filter read_filter(BitReader& r) {
    Filter f;
    f.kind = r.u8();
    switch (f.kind) {
    case 0: { // drop shadow
        f.color = read_rgba(r);
        f.blur_x = r.fixed16();
        f.blur_y = r.fixed16();
        f.angle = r.fixed16();
        f.distance = r.fixed16();
        f.strength = r.fixed8();
        const uint8_t b = r.u8();
        f.inner = b & 0x80;
        f.knockout = b & 0x40;
        f.composite = b & 0x20;
        f.passes = b & 0x1F;
        break;
    }
    case 1: { // blur
        f.blur_x = r.fixed16();
        f.blur_y = r.fixed16();
        f.passes = r.u8() >> 3;
        break;
    }
    case 2: { // glow
        f.color = read_rgba(r);
        f.blur_x = r.fixed16();
        f.blur_y = r.fixed16();
        f.strength = r.fixed8();
        const uint8_t b = r.u8();
        f.inner = b & 0x80;
        f.knockout = b & 0x40;
        f.composite = b & 0x20;
        f.passes = b & 0x1F;
        break;
    }
    case 3: { // bevel
        f.color = read_rgba(r); // shadow
        f.highlight = read_rgba(r);
        f.blur_x = r.fixed16();
        f.blur_y = r.fixed16();
        f.angle = r.fixed16();
        f.distance = r.fixed16();
        f.strength = r.fixed8();
        const uint8_t b = r.u8();
        f.inner = b & 0x80;
        f.knockout = b & 0x40;
        f.composite = b & 0x20;
        f.on_top = b & 0x10;
        f.passes = b & 0x0F;
        break;
    }
    case 4:
    case 7: { // gradient glow / bevel
        const unsigned n = r.u8();
        std::vector<uint32_t> colors(n);
        for (auto& c : colors) {
            c = read_rgba(r);
        }
        for (unsigned i = 0; i < n; ++i) {
            const uint8_t ratio = r.u8();
            f.values.push_back(float(ratio) / 255.0f);
            uint32_t c = colors[i];
            std::memcpy(&f.values.emplace_back(), &c, sizeof(float)); // colour bits, ratio pairs
        }
        f.blur_x = r.fixed16();
        f.blur_y = r.fixed16();
        f.angle = r.fixed16();
        f.distance = r.fixed16();
        f.strength = r.fixed8();
        const uint8_t b = r.u8();
        f.inner = b & 0x80;
        f.knockout = b & 0x40;
        f.composite = b & 0x20;
        f.on_top = b & 0x10;
        f.passes = b & 0x0F;
        break;
    }
    case 5: { // convolution
        const unsigned mx = r.u8(), my = r.u8();
        const float divisor = r.f32(), bias = r.f32();
        f.values.push_back(float(mx));
        f.values.push_back(float(my));
        f.values.push_back(divisor);
        f.values.push_back(bias);
        for (unsigned i = 0; i < mx * my; ++i) {
            f.values.push_back(r.f32());
        }
        f.color = read_rgba(r);
        r.u8(); // clamp, preserve alpha
        break;
    }
    case 6: // colour matrix
        for (int i = 0; i < 20; ++i) {
            f.values.push_back(r.f32());
        }
        break;
    default:
        throw ParseError(std::format("swf: unknown filter {}", f.kind));
    }
    return f;
}

std::vector<ClipAction> read_clip_actions(BitReader& r, uint8_t swf_version) {
    std::vector<ClipAction> out;
    r.u16(); // reserved
    if (swf_version >= 6) {
        r.u32();
    } else {
        r.u16();
    }
    for (;;) {
        const uint32_t events = swf_version >= 6 ? r.u32() : r.u16();
        if (events == 0) {
            break;
        }
        const uint32_t size = r.u32();
        ClipAction a;
        a.events = events;
        uint32_t body = size;
        if (events & 0x00020000) { // key press
            a.key = r.u8();
            body -= 1;
        }
        a.actions = r.take(body);
        out.push_back(a);
    }
    return out;
}

} // namespace

// ---- bits -------------------------------------------------------------------------------------

uint32_t BitReader::ubits(unsigned n) {
    uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (m_pos >= m_data.size()) {
            throw ParseError("swf: bit read past end");
        }
        const unsigned byte = static_cast<unsigned>(m_data[m_pos]);
        v = (v << 1) | ((byte >> (7 - m_bit)) & 1);
        if (++m_bit == 8) {
            m_bit = 0;
            ++m_pos;
        }
    }
    return v;
}

int32_t BitReader::sbits(unsigned n) {
    if (n == 0) {
        return 0;
    }
    const uint32_t v = ubits(n);
    if (n < 32 && (v & (1u << (n - 1)))) {
        return static_cast<int32_t>(v | (~0u << n));
    }
    return static_cast<int32_t>(v);
}

uint8_t BitReader::u8() {
    align();
    return static_cast<uint8_t>(take(1)[0]);
}

uint16_t BitReader::u16() {
    align();
    auto b = take(2);
    return static_cast<uint16_t>(unsigned(b[0]) | (unsigned(b[1]) << 8));
}

uint32_t BitReader::u32() {
    align();
    auto b = take(4);
    return unsigned(b[0]) | (unsigned(b[1]) << 8) | (unsigned(b[2]) << 16) | (unsigned(b[3]) << 24);
}

float BitReader::f32() {
    const uint32_t bits = u32();
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

std::string BitReader::cstr() {
    align();
    std::string s;
    for (;;) {
        const char c = static_cast<char>(take(1)[0]);
        if (c == '\0') {
            return s;
        }
        s += c;
    }
}

std::span<const std::byte> BitReader::take(size_t n) {
    align();
    if (n > remaining()) {
        throw ParseError(std::format("swf: read of {} bytes past end ({} left)", n, remaining()));
    }
    auto s = m_data.subspan(m_pos, n);
    m_pos += n;
    return s;
}

// ---- file -------------------------------------------------------------------------------------

File open(std::span<const std::byte> bytes) {
    if (bytes.size() < 8) {
        throw ParseError("swf: file too small");
    }
    File f;
    f.header.signature.assign(reinterpret_cast<const char*>(bytes.data()), 3);
    f.header.version = static_cast<uint8_t>(bytes[3]);
    uint32_t length = 0;
    std::memcpy(&length, bytes.data() + 4, 4);
    const auto& sig = f.header.signature;
    if (sig == "CWS" || sig == "CFX") {
        f.body = inflate_all(bytes.subspan(8), length > 8 ? length - 8 : 0);
    } else if (sig == "FWS" || sig == "GFX") {
        f.body.assign(bytes.begin() + 8, bytes.end());
    } else {
        throw ParseError("swf: not a Flash movie (signature " + sig + ")");
    }
    BitReader r(f.body);
    const Rect frame = read_rect(r);
    f.header.width = (frame.x1 - frame.x0) / 20.0f;
    f.header.height = (frame.y1 - frame.y0) / 20.0f;
    r.align();
    f.header.frame_rate = float(r.u16()) / 256.0f;
    f.header.frame_count = r.u16();
    f.tags = parse_tags(std::span(f.body).subspan(r.pos()));
    return f;
}

std::vector<TagData> parse_tags(std::span<const std::byte> data) {
    std::vector<TagData> out;
    BinaryReader r(data);
    while (r.tell() + 2 <= r.size()) {
        const uint16_t code_len = r.read<uint16_t>();
        const uint16_t code = code_len >> 6;
        uint32_t len = code_len & 0x3F;
        if (len == 0x3F) {
            len = r.read<uint32_t>();
        }
        out.push_back({code, r.take(len)});
        if (code == tag_end) {
            break;
        }
    }
    return out;
}

// ---- records ----------------------------------------------------------------------------------

Rect read_rect(BitReader& r) {
    r.align();
    const unsigned n = r.ubits(5);
    Rect rc;
    rc.x0 = float(r.sbits(n));
    rc.x1 = float(r.sbits(n));
    rc.y0 = float(r.sbits(n));
    rc.y1 = float(r.sbits(n));
    r.align();
    return rc;
}

Matrix read_matrix(BitReader& r) {
    r.align();
    Matrix m;
    if (r.ubits(1)) {
        const unsigned n = r.ubits(5);
        m.a = r.fbits(n);
        m.d = r.fbits(n);
    }
    if (r.ubits(1)) {
        const unsigned n = r.ubits(5);
        m.b = r.fbits(n);
        m.c = r.fbits(n);
    }
    const unsigned n = r.ubits(5);
    m.tx = float(r.sbits(n));
    m.ty = float(r.sbits(n));
    r.align();
    return m;
}

ColorTransform read_cxform(BitReader& r, bool with_alpha) {
    r.align();
    ColorTransform c;
    const bool has_add = r.ubits(1), has_mul = r.ubits(1);
    const unsigned n = r.ubits(4);
    const int channels = with_alpha ? 4 : 3;
    if (has_mul) {
        for (int i = 0; i < channels; ++i) {
            c.mul[i] = float(r.sbits(n)) / 256.0f;
        }
    }
    if (has_add) {
        for (int i = 0; i < channels; ++i) {
            c.add[i] = float(r.sbits(n));
        }
    }
    r.align();
    return c;
}

uint32_t read_rgb(BitReader& r) {
    const uint8_t cr = r.u8(), cg = r.u8(), cb = r.u8();
    return pack_rgba(cr, cg, cb, 255);
}

uint32_t read_rgba(BitReader& r) {
    const uint8_t cr = r.u8(), cg = r.u8(), cb = r.u8(), ca = r.u8();
    return pack_rgba(cr, cg, cb, ca);
}

Shape read_shape(BitReader& r, int version) {
    Shape s;
    if (version > 0) {
        read_styles(r, version, s);
    } else {
        s.fills.push_back(FillStyle{}); // glyphs: fill 1
        s.group_fill_start.push_back(0);
        s.group_line_start.push_back(0);
    }
    r.align();
    unsigned fill_bits = r.ubits(4), line_bits = r.ubits(4);
    uint32_t fill_base = s.group_fill_start.back(), line_base = s.group_line_start.back();
    int32_t x = 0, y = 0;
    uint32_t fill0 = 0, fill1 = 0, line = 0;
    auto style = [](uint32_t raw, uint32_t base) { return raw == 0 ? 0u : base + raw; };
    for (;;) {
        if (r.ubits(1) == 0) {
            const unsigned flags = r.ubits(5);
            if (flags == 0) {
                break;
            }
            if (flags & 0x01) { // move to
                const unsigned n = r.ubits(5);
                x = r.sbits(n);
                y = r.sbits(n);
            }
            if (flags & 0x02) {
                fill0 = style(r.ubits(fill_bits), fill_base);
            }
            if (flags & 0x04) {
                fill1 = style(r.ubits(fill_bits), fill_base);
            }
            if (flags & 0x08) {
                line = style(r.ubits(line_bits), line_base);
            }
            if (flags & 0x10) { // new styles: a new layer
                read_styles(r, version, s);
                r.align();
                fill_bits = r.ubits(4);
                line_bits = r.ubits(4);
                fill_base = s.group_fill_start.back();
                line_base = s.group_line_start.back();
            }
            continue;
        }
        Edge e{};
        e.x0 = x;
        e.y0 = y;
        e.fill0 = fill0;
        e.fill1 = fill1;
        e.line = line;
        if (r.ubits(1)) { // straight
            const unsigned n = r.ubits(4) + 2;
            int32_t dx = 0, dy = 0;
            if (r.ubits(1)) {
                dx = r.sbits(n);
                dy = r.sbits(n);
            } else if (r.ubits(1)) {
                dy = r.sbits(n);
            } else {
                dx = r.sbits(n);
            }
            e.curved = false;
            e.x1 = x + dx;
            e.y1 = y + dy;
            e.cx = (e.x0 + e.x1) / 2;
            e.cy = (e.y0 + e.y1) / 2;
        } else {
            const unsigned n = r.ubits(4) + 2;
            const int32_t cdx = r.sbits(n), cdy = r.sbits(n), adx = r.sbits(n), ady = r.sbits(n);
            e.curved = true;
            e.cx = x + cdx;
            e.cy = y + cdy;
            e.x1 = e.cx + adx;
            e.y1 = e.cy + ady;
        }
        x = e.x1;
        y = e.y1;
        s.edges.push_back(e);
    }
    r.align();
    return s;
}

std::vector<Filter> read_filters(BitReader& r) {
    std::vector<Filter> out;
    const unsigned n = r.u8();
    for (unsigned i = 0; i < n; ++i) {
        out.push_back(read_filter(r));
    }
    return out;
}

PlaceObject read_place_object(const TagData& tag, uint8_t swf_version) {
    BitReader r(tag.data);
    PlaceObject p;
    if (tag.code == tag_place_object) {
        p.version = 1;
        p.character = r.u16();
        p.depth = r.u16();
        p.matrix = read_matrix(r);
        if (r.remaining() > 0) {
            p.cxform = read_cxform(r, false);
        }
        return p;
    }
    p.version = tag.code == tag_place_object3 ? 3 : 2;
    const uint8_t f1 = r.u8();
    const uint8_t f2 = p.version == 3 ? r.u8() : 0;
    p.move = f1 & 0x01;
    p.depth = r.u16();
    if (p.version == 3 && ((f2 & 0x08) || ((f2 & 0x10) && (f1 & 0x02)))) {
        r.cstr(); // class name (AS3)
    }
    if (f1 & 0x02) {
        p.character = r.u16();
    }
    if (f1 & 0x04) {
        p.matrix = read_matrix(r);
    }
    if (f1 & 0x08) {
        p.cxform = read_cxform(r, true);
    }
    if (f1 & 0x10) {
        p.ratio = r.u16();
    }
    if (f1 & 0x20) {
        p.name = r.cstr();
    }
    if (f1 & 0x40) {
        p.clip_depth = r.u16();
    }
    if (p.version == 3) {
        if (f2 & 0x01) {
            p.filters = read_filters(r);
            p.has_filters = true;
        }
        if (f2 & 0x02) {
            p.blend = r.u8();
        }
        if (f2 & 0x04) {
            p.cache_as_bitmap = r.u8() != 0;
        }
        if (f2 & 0x20) {
            p.visible = r.u8() != 0;
        }
        if (f2 & 0x40) {
            read_rgba(r); // opaque background colour
        }
    }
    if (f1 & 0x80) {
        p.clip_actions = read_clip_actions(r, swf_version);
    }
    return p;
}

EditText read_edit_text(const TagData& tag) {
    BitReader r(tag.data);
    EditText t;
    t.id = r.u16();
    t.bounds = read_rect(r);
    const uint8_t f1 = r.u8(), f2 = r.u8();
    const bool has_text = f1 & 0x80;
    t.word_wrap = f1 & 0x40;
    t.multiline = f1 & 0x20;
    t.password = f1 & 0x10;
    t.read_only = f1 & 0x08;
    const bool has_color = f1 & 0x04, has_max = f1 & 0x02, has_font = f1 & 0x01;
    const bool has_font_class = f2 & 0x80;
    t.auto_size = f2 & 0x40;
    t.has_layout = f2 & 0x20;
    t.no_select = f2 & 0x10;
    t.border = f2 & 0x08;
    t.was_static = f2 & 0x04;
    t.html = f2 & 0x02;
    t.use_outlines = f2 & 0x01;
    if (has_font) {
        t.font = r.u16();
    }
    if (has_font_class) {
        t.font_class = r.cstr();
    }
    if (has_font || has_font_class) {
        t.height = r.u16();
    }
    if (has_color) {
        t.color = read_rgba(r);
    }
    if (has_max) {
        t.max_length = r.u16();
    }
    if (t.has_layout) {
        t.align = r.u8();
        t.left_margin = r.u16();
        t.right_margin = r.u16();
        t.indent = r.u16();
        t.leading = r.s16();
    }
    t.variable = r.cstr();
    if (has_text) {
        t.text = r.cstr();
    }
    return t;
}

Font read_font(const TagData& tag) {
    BitReader r(tag.data);
    Font f;
    f.id = r.u16();
    if (tag.code != tag_define_font2 && tag.code != tag_define_font3) {
        throw ParseError("swf: only DefineFont2/3 are supported");
    }
    f.units_per_em = tag.code == tag_define_font3 ? 20480.0f : 1024.0f;
    const uint8_t flags = r.u8();
    f.has_layout = flags & 0x80;
    const bool wide_offsets = flags & 0x08, wide_codes = flags & 0x04;
    f.small_text = flags & 0x20;
    f.italic = flags & 0x02;
    f.bold = flags & 0x01;
    r.u8(); // language
    const unsigned name_len = r.u8();
    auto name = r.take(name_len);
    f.name.assign(reinterpret_cast<const char*>(name.data()), name.size());
    while (!f.name.empty() && f.name.back() == '\0') {
        f.name.pop_back();
    }
    const unsigned glyphs = r.u16();
    const size_t table = r.pos();
    std::vector<uint32_t> offsets(glyphs + 1);
    for (unsigned i = 0; i < glyphs; ++i) {
        offsets[i] = wide_offsets ? r.u32() : r.u16();
    }
    const uint32_t code_offset = glyphs > 0 || r.remaining() > 0 ? (wide_offsets ? r.u32() : r.u16()) : 0;
    offsets[glyphs] = code_offset;
    for (unsigned i = 0; i < glyphs; ++i) {
        BitReader g(tag.data.subspan(table + offsets[i], offsets[i + 1] - offsets[i]));
        f.glyphs.push_back(read_shape(g, 0));
    }
    r.seek(table + code_offset);
    for (unsigned i = 0; i < glyphs; ++i) {
        f.codes.push_back(wide_codes ? r.u16() : r.u8());
    }
    if (f.has_layout) {
        f.ascent = r.u16();
        f.descent = r.u16();
        f.leading = r.s16();
        for (unsigned i = 0; i < glyphs; ++i) {
            f.advances.push_back(r.s16());
        }
        for (unsigned i = 0; i < glyphs; ++i) {
            f.bounds.push_back(read_rect(r));
        }
        const unsigned kerns = r.remaining() >= 2 ? r.u16() : 0;
        for (unsigned i = 0; i < kerns; ++i) {
            Font::Kern k;
            k.left = wide_codes ? r.u16() : r.u8();
            k.right = wide_codes ? r.u16() : r.u8();
            k.adjust = r.s16();
            f.kerning.push_back(k);
        }
    }
    return f;
}

Image read_bitmap(const TagData& tag, std::span<const std::byte> jpeg_tables) {
    BitReader r(tag.data);
    Image img;
    img.id = r.u16();
    if (tag.code == tag_define_bits_lossless || tag.code == tag_define_bits_lossless2) {
        const bool alpha = tag.code == tag_define_bits_lossless2;
        const uint8_t format = r.u8();
        img.width = r.u16();
        img.height = r.u16();
        const unsigned colors = format == 3 ? unsigned(r.u8()) + 1 : 0;
        const auto zdata = r.take(r.remaining());
        const size_t w = img.width, h = img.height;
        img.rgba.resize(w * h * 4);
        if (format == 3) {
            const size_t entry = alpha ? 4 : 3;
            const size_t stride = (w + 3) & ~size_t(3);
            const auto raw = inflate_all(zdata, colors * entry + stride * h);
            for (size_t y = 0; y < h; ++y) {
                for (size_t x = 0; x < w; ++x) {
                    const size_t at = colors * entry + y * stride + x;
                    const unsigned i = at < raw.size() ? unsigned(raw[at]) : 0;
                    uint8_t* px = &img.rgba[(y * w + x) * 4];
                    if (i < colors) {
                        const std::byte* c = &raw[i * entry];
                        const uint8_t a = alpha ? uint8_t(c[3]) : 255;
                        // Lossless2 colour maps are premultiplied already, like its ARGB pixels.
                        px[0] = uint8_t(c[0]);
                        px[1] = uint8_t(c[1]);
                        px[2] = uint8_t(c[2]);
                        px[3] = a;
                    }
                }
            }
        } else if (format == 5) {
            const auto raw = inflate_all(zdata, w * h * 4);
            for (size_t i = 0; i < w * h && i * 4 + 3 < raw.size(); ++i) {
                const uint8_t a = alpha ? uint8_t(raw[i * 4]) : 255;
                img.rgba[i * 4 + 0] = uint8_t(raw[i * 4 + 1]);
                img.rgba[i * 4 + 1] = uint8_t(raw[i * 4 + 2]);
                img.rgba[i * 4 + 2] = uint8_t(raw[i * 4 + 3]);
                img.rgba[i * 4 + 3] = a;
            }
        } else if (format == 4) {
            const size_t stride = (w * 2 + 3) & ~size_t(3);
            const auto raw = inflate_all(zdata, stride * h);
            for (size_t y = 0; y < h; ++y) {
                for (size_t x = 0; x < w; ++x) {
                    const size_t at = y * stride + x * 2;
                    const unsigned v = (unsigned(raw[at]) << 8) | unsigned(raw[at + 1]);
                    uint8_t* px = &img.rgba[(y * w + x) * 4];
                    px[0] = uint8_t(((v >> 10) & 31) * 255 / 31);
                    px[1] = uint8_t(((v >> 5) & 31) * 255 / 31);
                    px[2] = uint8_t((v & 31) * 255 / 31);
                    px[3] = 255;
                }
            }
        } else {
            throw ParseError(std::format("swf: lossless bitmap format {}", format));
        }
        return img;
    }
    // JPEG variants.
    uint32_t alpha_offset = 0;
    if (tag.code == tag_define_bits_jpeg3) {
        alpha_offset = r.u32();
    }
    std::span<const std::byte> jpeg = tag.code == tag_define_bits_jpeg3 ? r.take(alpha_offset) : r.take(r.remaining());
    std::vector<std::byte> joined;
    if (tag.code == tag_define_bits && !jpeg_tables.empty()) {
        // Tables and image data come in two streams; drop the tables' EOI and the image's SOI.
        auto strip = [](std::span<const std::byte> s, bool head) {
            if (head && s.size() >= 2 && s[0] == std::byte{0xFF} && s[1] == std::byte{0xD8}) {
                return s.subspan(2);
            }
            if (!head && s.size() >= 2 && s[s.size() - 2] == std::byte{0xFF} && s[s.size() - 1] == std::byte{0xD9}) {
                return s.first(s.size() - 2);
            }
            return s;
        };
        auto tables = strip(jpeg_tables, false);
        auto image = strip(jpeg, true);
        joined.assign(tables.begin(), tables.end());
        joined.insert(joined.end(), image.begin(), image.end());
        jpeg = joined;
    } else if (jpeg.size() >= 4 && jpeg[0] == std::byte{0xFF} && jpeg[1] == std::byte{0xD9} &&
               jpeg[2] == std::byte{0xFF} && jpeg[3] == std::byte{0xD8}) {
        jpeg = jpeg.subspan(4); // old encoders' leading EOI SOI
    }
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(jpeg.data()), static_cast<int>(jpeg.size()),
                                        &w, &h, &n, 4);
    if (!px) {
        throw ParseError(std::string("swf: JPEG decode failed: ") + stbi_failure_reason());
    }
    img.width = uint32_t(w);
    img.height = uint32_t(h);
    img.rgba.assign(px, px + size_t(w) * h * 4);
    stbi_image_free(px);
    if (tag.code == tag_define_bits_jpeg3 && r.remaining() > 0) {
        const auto a = inflate_all(r.take(r.remaining()), size_t(w) * h);
        for (size_t i = 0; i < size_t(w) * h && i < a.size(); ++i) {
            const unsigned alpha = unsigned(a[i]);
            for (int c = 0; c < 3; ++c) {
                img.rgba[i * 4 + c] = uint8_t((img.rgba[i * 4 + c] * alpha + 127) / 255);
            }
            img.rgba[i * 4 + 3] = uint8_t(alpha);
        }
    }
    return img;
}

Button read_button2(const TagData& tag) {
    BitReader r(tag.data);
    Button b;
    b.id = r.u16();
    b.track_as_menu = r.u8() & 1;
    const size_t action_at = r.pos();
    const uint16_t action_offset = r.u16();
    for (;;) {
        const uint8_t flags = r.u8();
        if (flags == 0) {
            break;
        }
        ButtonRecord rec;
        rec.states = flags & 0x0F;
        rec.character = r.u16();
        rec.depth = r.u16();
        rec.matrix = read_matrix(r);
        rec.cxform = read_cxform(r, true);
        if (flags & 0x10) {
            rec.filters = read_filters(r);
        }
        if (flags & 0x20) {
            rec.blend = r.u8();
        }
        b.records.push_back(std::move(rec));
    }
    if (action_offset != 0) {
        size_t pos = action_at + action_offset;
        for (;;) {
            r.seek(pos);
            const uint16_t size = r.u16();
            Button::Action a;
            a.conditions = r.u16();
            a.actions = size ? tag.data.subspan(pos + 4, size - 4) : tag.data.subspan(pos + 4);
            b.actions.push_back(a);
            if (size == 0) {
                break;
            }
            pos += size;
        }
    }
    return b;
}

} // namespace spud::legacy::swf
