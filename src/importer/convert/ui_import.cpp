#include "importer/convert/ui_import.hpp"

#include "importer/convert/mesh_import.hpp"
#include "importer/convert/particle_import.hpp"
#include "importer/convert/ui_controls.hpp"

#include "importer/convert/ui_script.hpp"
#include "importer/convert/ui_shapes.hpp"
#include "importer/legacy/data_source.hpp"
#include "importer/legacy/swf.hpp"

#include "core/error.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "formats/osm.hpp"
#include "formats/ose.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <unordered_map>

namespace spud::importer {

namespace {

namespace swf = legacy::swf;

constexpr float k_twips = 20.0f;
// Curve flattening: 1/40 of a stage pixel, so curves stay smooth up to the 3x scale of a 4K screen.
constexpr float k_shape_tolerance = 0.5f; // twips
constexpr float k_glyph_tolerance_em = 0.0004f;

osu::Matrix to_pixels(const swf::Matrix& m) {
    return {m.a, m.b, m.c, m.d, m.tx / k_twips, m.ty / k_twips};
}

osu::Rect to_pixels(const swf::Rect& r) {
    return {r.x0 / k_twips, r.y0 / k_twips, r.x1 / k_twips, r.y1 / k_twips};
}

osu::ColorTransform to_osu(const swf::ColorTransform& c) {
    osu::ColorTransform o;
    for (int i = 0; i < 4; ++i) {
        o.mul[i] = c.mul[i];
        o.add[i] = c.add[i] / 255.0f;
    }
    return o;
}

// Inverse of an affine matrix.
osu::Matrix inverse(const swf::Matrix& m) {
    const double det = double(m.a) * m.d - double(m.b) * m.c;
    if (std::abs(det) < 1e-12) {
        return {0, 0, 0, 0, 0, 0};
    }
    const double ia = m.d / det, ib = -m.b / det, ic = -m.c / det, id = m.a / det;
    const double itx = -(ia * m.tx + ic * m.ty), ity = -(ib * m.tx + id * m.ty);
    return {float(ia), float(ib), float(ic), float(id), float(itx), float(ity)};
}

// out = scale(sx, sy) * inv * scale(k_twips): stage pixels -> fill space.
osu::Matrix fill_space(const swf::Matrix& m, float sx, float sy) {
    const osu::Matrix i = inverse(m);
    return {i.a * k_twips * sx, i.b * k_twips * sy, i.c * k_twips * sx, i.d * k_twips * sy, i.tx * sx, i.ty * sy};
}

class Converter {
public:
    Converter() : m_script(m_movie, m_strings) {}

    MovieImport run(std::span<const std::byte> bytes) {
        swf::File file = swf::open(bytes);
        m_version = file.header.version;
        auto& h = m_movie.header;
        h.stage_width = file.header.width;
        h.stage_height = file.header.height;
        h.frame_rate = file.header.frame_rate;
        h.source_version = file.header.version;
        h.background = 0xFF000000u;
        m_movie.sprites.emplace_back(); // root timeline
        h.root_sprite = 0;
        timeline(file.tags, 0);
        MovieImport out;
        out.movie = std::move(m_movie);
        auto bytes_view = m_strings.bytes();
        out.movie.strings.assign(reinterpret_cast<const char*>(bytes_view.data()),
                                 reinterpret_cast<const char*>(bytes_view.data()) + bytes_view.size());
        out.warnings = std::move(m_warnings);
        for (const auto& w : m_script.warnings()) {
            out.warnings.push_back(w);
        }
        return out;
    }

private:
    uint32_t str(std::string_view s) { return m_strings.add(s); }

    uint32_t add_character(uint16_t id, osu::CharKind kind, uint32_t index) {
        const auto ci = static_cast<uint32_t>(m_movie.characters.size());
        m_movie.characters.push_back({id, kind, 0, index});
        m_by_id[id] = ci;
        return ci;
    }

    uint32_t character(uint16_t id) {
        auto it = m_by_id.find(id);
        if (it == m_by_id.end()) {
            m_warnings.push_back(std::format("reference to undefined character {}", id));
            return osu::k_none;
        }
        return it->second;
    }

    void warn_once(const std::string& w) {
        if (std::find(m_warnings.begin(), m_warnings.end(), w) == m_warnings.end()) {
            m_warnings.push_back(w);
        }
    }

    uint32_t add_filters(const std::vector<swf::Filter>& filters) {
        const auto first = static_cast<uint32_t>(m_movie.filters.size());
        for (const auto& f : filters) {
            osu::Filter o{};
            o.kind = static_cast<osu::FilterKind>(f.kind);
            o.passes = f.passes;
            o.flags = uint8_t((f.inner ? osu::filter_inner : 0) | (f.knockout ? osu::filter_knockout : 0) |
                              (f.composite ? osu::filter_composite : 0) | (f.on_top ? osu::filter_on_top : 0));
            o.color = f.color;
            o.highlight = f.highlight;
            o.blur_x = f.blur_x;
            o.blur_y = f.blur_y;
            o.strength = f.strength;
            o.angle = f.angle;
            o.distance = f.distance;
            o.first_value = static_cast<uint32_t>(m_movie.filter_values.size());
            o.value_count = static_cast<uint32_t>(f.values.size());
            m_movie.filter_values.insert(m_movie.filter_values.end(), f.values.begin(), f.values.end());
            m_movie.filters.push_back(o);
        }
        return first;
    }

    void shape(const swf::TagData& tag) {
        const int version = tag.code == swf::tag_define_shape    ? 1
                            : tag.code == swf::tag_define_shape2 ? 2
                            : tag.code == swf::tag_define_shape3 ? 3
                                                                 : 4;
        swf::BitReader r(tag.data);
        const uint16_t id = r.u16();
        const swf::Rect bounds = swf::read_rect(r);
        if (version == 4) {
            swf::read_rect(r); // edge bounds
            r.u8();            // stroke hints
        }
        const swf::Shape s = swf::read_shape(r, version);
        osu::Shape out{};
        out.bounds = to_pixels(bounds);
        out.first_mesh = static_cast<uint32_t>(m_movie.meshes.size());
        const ShapeMeshes parts = tessellate_shape(s, k_shape_tolerance);
        for (const auto& part : parts.parts) {
            osu::Mesh mesh{};
            const swf::FillStyle* fill = nullptr;
            swf::FillStyle solid;
            if (part.stroke) {
                const auto& l = s.lines[part.style];
                mesh.flags |= osu::mesh_stroke;
                if (l.has_fill && l.fill.type != 0) {
                    fill = &l.fill;
                } else {
                    solid.color = l.color;
                    fill = &solid;
                }
            } else {
                fill = &s.fills[part.style];
            }
            fill_mesh(*fill, mesh);
            mesh.first_vertex = static_cast<uint32_t>(m_movie.vertices.size());
            mesh.vertex_count = static_cast<uint32_t>(part.mesh.xy.size() / 2);
            for (size_t i = 0; i + 1 < part.mesh.xy.size(); i += 2) {
                m_movie.vertices.push_back({part.mesh.xy[i] / k_twips, part.mesh.xy[i + 1] / k_twips});
            }
            mesh.first_index = static_cast<uint32_t>(m_movie.indices.size());
            mesh.index_count = static_cast<uint32_t>(part.mesh.indices.size());
            m_movie.indices.insert(m_movie.indices.end(), part.mesh.indices.begin(), part.mesh.indices.end());
            m_movie.meshes.push_back(mesh);
        }
        out.mesh_count = static_cast<uint32_t>(m_movie.meshes.size()) - out.first_mesh;
        add_character(id, osu::CharKind::shape, static_cast<uint32_t>(m_movie.shapes.size()));
        m_movie.shapes.push_back(out);
    }

    void fill_mesh(const swf::FillStyle& f, osu::Mesh& mesh) {
        mesh.gradient = osu::k_none;
        mesh.bitmap = osu::k_none;
        if (f.type == 0x00) {
            mesh.fill = osu::Fill::solid;
            mesh.color = f.color;
        } else if (f.type == 0x10 || f.type == 0x12 || f.type == 0x13) {
            mesh.fill = f.type == 0x10 ? osu::Fill::linear : f.type == 0x12 ? osu::Fill::radial : osu::Fill::focal;
            mesh.fill_matrix = fill_space(f.matrix, 1.0f / 16384.0f, 1.0f / 16384.0f);
            osu::Gradient g{};
            g.spread = static_cast<osu::Spread>(std::min<uint8_t>(f.spread, 2));
            g.linear_rgb = f.interpolation == 1;
            g.stop_count = static_cast<uint8_t>(f.gradient.size());
            g.focal = f.focal;
            g.first_stop = static_cast<uint32_t>(m_movie.gradient_stops.size());
            for (const auto& s : f.gradient) {
                m_movie.gradient_stops.push_back({float(s.ratio) / 255.0f, s.color});
            }
            mesh.gradient = static_cast<uint32_t>(m_movie.gradients.size());
            m_movie.gradients.push_back(g);
        } else {
            mesh.fill = osu::Fill::bitmap;
            mesh.flags |= (f.type == 0x40 || f.type == 0x42) ? osu::mesh_bitmap_repeat : 0;
            mesh.flags |= (f.type == 0x40 || f.type == 0x41) ? osu::mesh_bitmap_smooth : 0;
            auto it = m_by_id.find(f.bitmap);
            if (f.bitmap == 0xFFFF || it == m_by_id.end()) {
                mesh.fill = osu::Fill::solid; // no bitmap: Flash draws nothing visible
                mesh.color = 0;
                return;
            }
            mesh.bitmap = it->second;
            const auto& b = m_movie.bitmaps[m_movie.characters[it->second].index];
            mesh.fill_matrix = fill_space(f.matrix, 1.0f / float(std::max(b.width, 1u)), 1.0f / float(std::max(b.height, 1u)));
        }
    }

    void bitmap(const swf::TagData& tag) {
        swf::Image img = swf::read_bitmap(tag, m_jpeg_tables);
        osu::Bitmap b{};
        b.width = img.width;
        b.height = img.height;
        b.offset = m_movie.bitmap_data.size();
        const auto* p = reinterpret_cast<const std::byte*>(img.rgba.data());
        m_movie.bitmap_data.insert(m_movie.bitmap_data.end(), p, p + img.rgba.size());
        add_character(img.id, osu::CharKind::bitmap, static_cast<uint32_t>(m_movie.bitmaps.size()));
        m_movie.bitmaps.push_back(b);
    }

    void font(const swf::TagData& tag) {
        const swf::Font f = swf::read_font(tag);
        const float s = 1.0f / f.units_per_em;
        osu::Font out{};
        out.name = str(f.name);
        out.flags = (f.bold ? osu::font_bold : 0) | (f.italic ? osu::font_italic : 0) |
                    (f.small_text ? osu::font_small : 0);
        out.ascent = f.ascent * s;
        out.descent = f.descent * s;
        out.leading = f.leading * s;
        out.first_glyph = static_cast<uint32_t>(m_movie.glyphs.size());
        std::vector<size_t> order(f.glyphs.size());
        for (size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return f.codes[a] < f.codes[b]; });
        for (size_t i : order) {
            osu::Glyph g{};
            g.code = f.codes[i];
            g.advance = i < f.advances.size() ? f.advances[i] * s : 0.0f;
            if (i < f.bounds.size()) {
                g.bounds = {f.bounds[i].x0 * s, f.bounds[i].y0 * s, f.bounds[i].x1 * s, f.bounds[i].y1 * s};
            }
            const ShapeMeshes parts = tessellate_shape(f.glyphs[i], k_glyph_tolerance_em * f.units_per_em);
            g.first_vertex = static_cast<uint32_t>(m_movie.vertices.size());
            g.first_index = static_cast<uint32_t>(m_movie.indices.size());
            uint32_t base = 0;
            for (const auto& part : parts.parts) {
                for (size_t k = 0; k + 1 < part.mesh.xy.size(); k += 2) {
                    m_movie.vertices.push_back({part.mesh.xy[k] * s, part.mesh.xy[k + 1] * s});
                }
                for (uint32_t idx : part.mesh.indices) {
                    m_movie.indices.push_back(idx + base);
                }
                base += static_cast<uint32_t>(part.mesh.xy.size() / 2);
            }
            g.vertex_count = base;
            g.index_count = static_cast<uint32_t>(m_movie.indices.size()) - g.first_index;
            m_movie.glyphs.push_back(g);
        }
        out.glyph_count = static_cast<uint32_t>(m_movie.glyphs.size()) - out.first_glyph;
        out.first_kerning = static_cast<uint32_t>(m_movie.kernings.size());
        for (const auto& k : f.kerning) {
            m_movie.kernings.push_back({k.left, k.right, k.adjust * s});
        }
        out.kerning_count = static_cast<uint32_t>(f.kerning.size());
        add_character(f.id, osu::CharKind::font, static_cast<uint32_t>(m_movie.fonts.size()));
        m_movie.fonts.push_back(out);
    }

    void edit_text(const swf::TagData& tag) {
        const swf::EditText t = swf::read_edit_text(tag);
        osu::EditText o{};
        o.bounds = to_pixels(t.bounds);
        o.flags = (t.word_wrap ? osu::text_word_wrap : 0) | (t.multiline ? osu::text_multiline : 0) |
                  (t.password ? osu::text_password : 0) | (t.read_only ? osu::text_read_only : 0) |
                  (t.auto_size ? osu::text_auto_size : 0) | (t.no_select ? osu::text_no_select : 0) |
                  (t.border ? osu::text_border : 0) | (t.was_static ? osu::text_was_static : 0) |
                  (t.html ? osu::text_html : 0) | (t.use_outlines ? osu::text_use_outlines : 0) |
                  (t.color ? osu::text_has_color : 0) | (t.has_layout ? osu::text_has_layout : 0);
        o.font = t.font ? character(*t.font) : osu::k_none;
        o.font_class = t.font_class ? str(*t.font_class) : osu::k_none;
        o.height = t.height / k_twips;
        o.color = t.color.value_or(0xFF000000u);
        o.max_length = t.max_length;
        o.align = t.align;
        o.left_margin = t.left_margin / k_twips;
        o.right_margin = t.right_margin / k_twips;
        o.indent = t.indent / k_twips;
        o.leading = t.leading / k_twips;
        o.variable = t.variable.empty() ? osu::k_none : str(t.variable);
        o.initial_text = t.text ? str(*t.text) : osu::k_none;
        add_character(t.id, osu::CharKind::text, static_cast<uint32_t>(m_movie.texts.size()));
        m_movie.texts.push_back(o);
    }

    void button(const swf::TagData& tag) {
        const swf::Button b = swf::read_button2(tag);
        osu::Button o{};
        o.track_as_menu = b.track_as_menu;
        o.first_record = static_cast<uint32_t>(m_movie.button_records.size());
        for (const auto& r : b.records) {
            osu::ButtonRecord rec{};
            rec.states = r.states;
            rec.blend = static_cast<osu::Blend>(r.blend > 1 ? r.blend - 1 : 0);
            rec.depth = r.depth;
            rec.character = character(r.character);
            rec.matrix = to_pixels(r.matrix);
            rec.cxform = to_osu(r.cxform);
            rec.first_filter = add_filters(r.filters);
            rec.filter_count = static_cast<uint32_t>(r.filters.size());
            m_movie.button_records.push_back(rec);
        }
        o.record_count = static_cast<uint32_t>(b.records.size());
        o.first_action = static_cast<uint32_t>(m_movie.button_actions.size());
        for (const auto& a : b.actions) {
            m_movie.button_actions.push_back({a.conditions, m_script.block(a.actions)});
        }
        o.action_count = static_cast<uint32_t>(b.actions.size());
        add_character(b.id, osu::CharKind::button, static_cast<uint32_t>(m_movie.buttons.size()));
        m_movie.buttons.push_back(o);
    }

    void place(const swf::TagData& tag, std::vector<osu::PlaceCommand>& frame) {
        const swf::PlaceObject p = swf::read_place_object(tag, m_version);
        osu::PlaceCommand c{};
        c.depth = p.depth;
        c.character = osu::k_none;
        c.name = osu::k_none;
        c.visible = 1;
        if (p.move) {
            c.flags |= osu::place_move;
        }
        if (p.character) {
            c.flags |= osu::place_character;
            c.character = character(*p.character);
        }
        if (p.matrix) {
            c.flags |= osu::place_matrix;
            c.matrix = to_pixels(*p.matrix);
        }
        if (p.cxform) {
            c.flags |= osu::place_cxform;
            c.cxform = to_osu(*p.cxform);
        }
        if (p.ratio) {
            c.flags |= osu::place_ratio;
            c.ratio = *p.ratio;
        }
        if (p.name) {
            c.flags |= osu::place_name;
            c.name = str(*p.name);
        }
        if (p.clip_depth) {
            c.flags |= osu::place_clip_depth;
            c.clip_depth = *p.clip_depth;
        }
        if (p.has_filters) {
            c.flags |= osu::place_filters;
            c.first_filter = add_filters(p.filters);
            c.filter_count = static_cast<uint32_t>(p.filters.size());
        }
        if (p.blend) {
            c.flags |= osu::place_blend;
            c.blend = static_cast<osu::Blend>(*p.blend > 1 ? *p.blend - 1 : 0);
        }
        if (p.cache_as_bitmap) {
            c.flags |= *p.cache_as_bitmap ? osu::place_cache_bitmap : 0;
        }
        if (p.visible) {
            c.flags |= osu::place_visible;
            c.visible = *p.visible;
        }
        if (!p.clip_actions.empty()) {
            c.flags |= osu::place_events;
            c.first_event = static_cast<uint32_t>(m_movie.events.size());
            for (const auto& a : p.clip_actions) {
                m_movie.events.push_back({a.events, a.key, {}, m_script.block(a.actions)});
            }
            c.event_count = static_cast<uint32_t>(p.clip_actions.size());
        }
        frame.push_back(c);
    }

    // Definitions and the control tags of one timeline (the root or a sprite).
    void timeline(const std::vector<swf::TagData>& tags, uint32_t sprite_index) {
        struct Pending {
            std::vector<osu::PlaceCommand> commands;
            std::vector<osu::FrameAction> actions;
            std::vector<osu::InitAction> inits;
            std::vector<std::string> labels;
        };
        std::vector<Pending> frames;
        Pending cur;
        for (const auto& tag : tags) {
            try {
                switch (tag.code) {
                case swf::tag_end:
                    break;
                case swf::tag_show_frame:
                    frames.push_back(std::move(cur));
                    cur = {};
                    break;
                case swf::tag_define_shape:
                case swf::tag_define_shape2:
                case swf::tag_define_shape3:
                case swf::tag_define_shape4:
                    shape(tag);
                    break;
                case swf::tag_jpeg_tables:
                    m_jpeg_tables.assign(tag.data.begin(), tag.data.end());
                    break;
                case swf::tag_define_bits:
                case swf::tag_define_bits_jpeg2:
                case swf::tag_define_bits_jpeg3:
                case swf::tag_define_bits_lossless:
                case swf::tag_define_bits_lossless2:
                    bitmap(tag);
                    break;
                case swf::tag_define_font2:
                case swf::tag_define_font3:
                    font(tag);
                    break;
                case swf::tag_define_edit_text:
                    edit_text(tag);
                    break;
                case swf::tag_define_button2:
                    button(tag);
                    break;
                case swf::tag_define_sprite: {
                    swf::BitReader r(tag.data);
                    const uint16_t id = r.u16();
                    r.u16(); // frame count (recomputed from the tags)
                    const auto inner = swf::parse_tags(tag.data.subspan(4));
                    const auto index = static_cast<uint32_t>(m_movie.sprites.size());
                    m_movie.sprites.emplace_back();
                    m_movie.sprites.back().scale9 = osu::k_none;
                    add_character(id, osu::CharKind::sprite, index);
                    timeline(inner, index);
                    break;
                }
                case swf::tag_place_object:
                case swf::tag_place_object2:
                case swf::tag_place_object3:
                    place(tag, cur.commands);
                    break;
                case swf::tag_remove_object:
                case swf::tag_remove_object2: {
                    swf::BitReader r(tag.data);
                    if (tag.code == swf::tag_remove_object) {
                        r.u16();
                    }
                    osu::PlaceCommand c{};
                    c.flags = osu::place_remove;
                    c.depth = r.u16();
                    c.character = osu::k_none;
                    c.name = osu::k_none;
                    cur.commands.push_back(c);
                    break;
                }
                case swf::tag_frame_label: {
                    swf::BitReader r(tag.data);
                    cur.labels.push_back(r.cstr());
                    break;
                }
                case swf::tag_do_action:
                    cur.actions.push_back({m_script.block(tag.data), static_cast<uint32_t>(cur.commands.size())});
                    break;
                case swf::tag_do_init_action: {
                    swf::BitReader r(tag.data);
                    const uint16_t id = r.u16();
                    cur.inits.push_back({character(id), m_script.block(tag.data.subspan(2))});
                    break;
                }
                case swf::tag_export_assets: {
                    swf::BitReader r(tag.data);
                    const unsigned n = r.u16();
                    for (unsigned i = 0; i < n; ++i) {
                        const uint16_t id = r.u16();
                        const std::string name = r.cstr();
                        m_movie.exports.push_back({str(name), character(id)});
                    }
                    break;
                }
                case swf::tag_import_assets:
                case swf::tag_import_assets2: {
                    swf::BitReader r(tag.data);
                    const std::string url = r.cstr();
                    if (tag.code == swf::tag_import_assets2) {
                        r.u8();
                        r.u8();
                    }
                    const unsigned n = r.u16();
                    for (unsigned i = 0; i < n; ++i) {
                        const uint16_t id = r.u16();
                        const std::string name = r.cstr();
                        add_character(id, osu::CharKind::imported, static_cast<uint32_t>(m_movie.imports.size()));
                        m_movie.imports.push_back({str(url), str(name)});
                    }
                    break;
                }
                case swf::tag_define_scaling_grid: {
                    swf::BitReader r(tag.data);
                    const uint16_t id = r.u16();
                    const swf::Rect rc = swf::read_rect(r);
                    const uint32_t ci = character(id);
                    if (ci != osu::k_none && m_movie.characters[ci].kind == osu::CharKind::sprite) {
                        m_movie.sprites[m_movie.characters[ci].index].scale9 = static_cast<uint32_t>(m_movie.rects.size());
                        m_movie.rects.push_back(to_pixels(rc));
                    }
                    break;
                }
                case swf::tag_set_background_color: {
                    swf::BitReader r(tag.data);
                    m_movie.header.background = swf::read_rgb(r);
                    break;
                }
                // Nothing the player needs: metadata, file attributes, font name/alignment hints, text settings.
                case 69: case 77: case 73: case 88: case 74: case 65: case 24: case 58: case 64: case 86:
                    break;
                default:
                    if (tag.code >= 1000) {
                        warn_once(std::format("Scaleform tag {} ignored", tag.code));
                    } else {
                        warn_once(std::format("tag {} not supported", tag.code));
                    }
                    break;
                }
            } catch (const std::exception& e) {
                m_warnings.push_back(std::format("tag {}: {}", tag.code, e.what()));
            }
        }
        if (!cur.commands.empty() || !cur.actions.empty() || !cur.inits.empty() || !cur.labels.empty()) {
            frames.push_back(std::move(cur));
        }
        osu::Sprite& sp = m_movie.sprites[sprite_index];
        sp.first_frame = static_cast<uint32_t>(m_movie.frames.size());
        sp.frame_count = static_cast<uint32_t>(frames.size());
        sp.first_label = static_cast<uint32_t>(m_movie.labels.size());
        if (sprite_index == 0) {
            sp.scale9 = osu::k_none;
        }
        for (uint32_t i = 0; i < frames.size(); ++i) {
            auto& f = frames[i];
            osu::Frame fr{};
            fr.first_command = static_cast<uint32_t>(m_movie.commands.size());
            fr.command_count = static_cast<uint32_t>(f.commands.size());
            m_movie.commands.insert(m_movie.commands.end(), f.commands.begin(), f.commands.end());
            fr.first_action = static_cast<uint32_t>(m_movie.frame_actions.size());
            fr.action_count = static_cast<uint32_t>(f.actions.size());
            m_movie.frame_actions.insert(m_movie.frame_actions.end(), f.actions.begin(), f.actions.end());
            fr.first_init = static_cast<uint32_t>(m_movie.init_actions.size());
            fr.init_count = static_cast<uint32_t>(f.inits.size());
            m_movie.init_actions.insert(m_movie.init_actions.end(), f.inits.begin(), f.inits.end());
            for (const auto& l : f.labels) {
                m_movie.labels.push_back({sprite_index, i, str(l)});
            }
            m_movie.frames.push_back(fr);
        }
        // Labels were appended after nested sprites' labels; this sprite's are the tail.
        m_movie.sprites[sprite_index].label_count = static_cast<uint32_t>(m_movie.labels.size()) - sp.first_label;
    }

    osu::Movie m_movie;
    container::StringTableBuilder m_strings;
    ScriptTranslator m_script;
    std::unordered_map<uint16_t, uint32_t> m_by_id;
    std::vector<std::byte> m_jpeg_tables;
    std::vector<std::string> m_warnings;
    uint8_t m_version = 0;
};

std::string utf16_to_utf8(std::span<const std::byte> b) {
    std::string out;
    size_t i = 0;
    if (b.size() >= 2 && b[0] == std::byte{0xFF} && b[1] == std::byte{0xFE}) {
        i = 2;
    }
    for (; i + 1 < b.size(); i += 2) {
        uint32_t c = unsigned(b[i]) | (unsigned(b[i + 1]) << 8);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < b.size()) {
            const uint32_t lo = unsigned(b[i + 2]) | (unsigned(b[i + 3]) << 8);
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::vector<char> to_chars(const container::StringTableBuilder& s) {
    auto b = s.bytes();
    return {reinterpret_cast<const char*>(b.data()), reinterpret_cast<const char*>(b.data()) + b.size()};
}

} // namespace

MovieImport import_movie(std::span<const std::byte> swf) { return Converter().run(swf); }

osu::Strings import_translations(std::span<const std::byte> text) {
    const std::string all = utf16_to_utf8(text);
    container::StringTableBuilder strings;
    std::map<std::string, std::string> entries;
    size_t start = 0;
    while (start < all.size()) {
        size_t end = all.find('\n', start);
        if (end == std::string::npos) {
            end = all.size();
        }
        std::string line = all.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t tab = line.find('\t');
        if (line.empty() || line[0] != '$' || tab == std::string::npos) {
            continue;
        }
        entries[line.substr(0, tab)] = line.substr(tab + 1);
    }
    osu::Strings out;
    for (const auto& [k, v] : entries) {
        out.entries.push_back({strings.add(k), strings.add(v)});
    }
    out.strings = to_chars(strings);
    return out;
}

osu::FontMap import_font_config(std::span<const std::byte> text) {
    const std::string all(reinterpret_cast<const char*>(text.data()), text.size());
    container::StringTableBuilder strings;
    osu::FontMap out;
    size_t start = 0;
    auto quoted = [](const std::string& s, size_t& pos) {
        const size_t a = s.find('"', pos);
        const size_t b = a == std::string::npos ? a : s.find('"', a + 1);
        if (b == std::string::npos) {
            return std::string();
        }
        pos = b + 1;
        return s.substr(a + 1, b - a - 1);
    };
    while (start < all.size()) {
        size_t end = all.find('\n', start);
        if (end == std::string::npos) {
            end = all.size();
        }
        std::string line = all.substr(start, end - start);
        start = end + 1;
        size_t pos = 0;
        if (line.starts_with("fontlib")) {
            std::string lib = path::to_lower(quoted(line, pos));
            std::replace(lib.begin(), lib.end(), '\\', '/');
            if (lib.ends_with(".swf")) {
                lib = lib.substr(0, lib.size() - 4) + ".osu";
            }
            out.libraries.push_back(strings.add(lib));
        } else if (line.starts_with("map")) {
            const std::string logical = quoted(line, pos);
            const std::string font = quoted(line, pos);
            const std::string style = path::to_lower(line.substr(pos));
            uint32_t flags = 0;
            if (style.find("bold") != std::string::npos) {
                flags |= osu::font_bold;
            }
            if (style.find("italic") != std::string::npos) {
                flags |= osu::font_italic;
            }
            out.names.push_back({strings.add(logical), strings.add(font), flags});
        }
    }
    out.strings = to_chars(strings);
    return out;
}

namespace {

// True if `path` holds a movie in the current format version (older ones are converted again).
bool current_movie(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    container::FileHeader h{};
    return in.read(reinterpret_cast<char*>(&h), sizeof(h)) && h.kind == osu::k_kind_movie &&
           h.version == osu::k_version;
}

} // namespace

bool convert_scene_model(const legacy::DataSource& data, const std::string& cache_dir, std::string_view model,
                         bool force) {
    const std::filesystem::path cache(cache_dir);
    const std::string src = path::with_root(path::normalize(model), "meshes");
    const auto out = cache / path::replace_extension(src, ".osm");
    const auto out_graph = cache / path::replace_extension(src, ".ose");
    if (!force && std::filesystem::exists(out) && std::filesystem::exists(out_graph)) {
        // A scene graph written before version 3 lacks what the UI scenes read, and a mesh older than the oldest
        // version the engine reads doesn't load: convert them again.
        try {
            if (ose::read(read_file(out_graph)).node_extras.size() > 0) {
                osm::read(read_file(out));
                return true;
            }
        } catch (const std::exception&) {
        }
    }
    try {
        auto nif = data.read(src);
        if (!nif) {
            log::warn("ui: {} not found", src);
            return false;
        }
        const legacy::NifFile parsed = legacy::read_nif(*nif);
        auto mesh = import_nif(parsed, src);
        auto graph = import_scene_model(parsed, *nif, src, mesh.submesh_shapes);
        for (const auto& w : mesh.warnings) {
            log::debug("ui: {}: {}", src, w);
        }
        for (const auto& w : graph.warnings) {
            log::debug("ui: {}: {}", src, w);
        }
        write_file(out, osm::write(mesh.mesh));
        write_file(out_graph, ose::write(graph.effect));
        // DDS is a GPU format: textures are extracted as they are, as for world meshes.
        std::vector<std::string> textures = mesh.textures;
        textures.insert(textures.end(), graph.textures.begin(), graph.textures.end());
        for (const auto& t : textures) {
            const auto tex = cache / t;
            if (!force && std::filesystem::exists(tex)) {
                continue;
            }
            if (auto bytes = data.read(t)) {
                write_file(tex, *bytes);
            } else {
                log::warn("ui: {}: texture {} not found", src, t);
            }
        }
        return true;
    } catch (const std::exception& e) {
        log::warn("ui: {}: {}", src, e.what());
        return false;
    }
}

size_t convert_world_map(const legacy::DataSource& data, const std::string& cache_dir, bool force) {
    const std::filesystem::path cache(cache_dir);
    constexpr const char* k_rock = "textures/terrain/worldmapoverlay_n.dds";     // sWorldMapOverlayNormalTexture
    constexpr const char* k_snow = "textures/terrain/worldmapoverlaysnow_n.dds"; // sWorldMapOverlayNormalSnowTexture
    constexpr const char* k_noise = "textures/terrain/noise.dds";
    auto texture = [&](const std::string& t) {
        if (const auto out = cache / t; force || !std::filesystem::exists(out)) {
            if (auto bytes = data.read(t)) {
                write_file(out, *bytes);
            } else {
                log::warn("world map: texture {} not found", t);
            }
        }
    };
    for (const char* t : {k_rock, k_snow, k_noise}) {
        texture(t);
    }
    size_t blocks = 0;
    auto convert = [&](const std::string& src) {
        const auto out = cache / (src + ".osm");
        try {
            if (!force && std::filesystem::exists(out)) {
                ++blocks;
                return;
            }
            auto nif = data.read(src);
            if (!nif) {
                return;
            }
            auto mesh = import_nif(legacy::read_nif(*nif), src);
            for (osm::Material& m : mesh.mesh.materials) {
                m.params.skyrim_flags2 |= 1u << 23;
                m.textures[osm::texture_env_mask] = k_rock;
                m.textures[osm::texture_inner_layer] = k_snow;
                m.textures[osm::texture_backlight] = k_noise;
            }
            write_file(out, osm::write(mesh.mesh));
            for (const auto& t : mesh.textures) {
                texture(t);
            }
            ++blocks;
        } catch (const std::exception& e) {
            log::warn("world map: {}: {}", src, e.what());
        }
    };
    for (const auto& file : data.list("meshes/terrain/", ".btr")) {
        if (file.find(".32.") != std::string::npos) {
            convert(file);
        }
    }
    for (const auto& file : data.list("meshes/terrain/", ".bto")) {
        if (file.find(".16.") != std::string::npos) {
            convert(file);
        }
    }
    convert_scene_model(data, cache_dir, "meshes/sky/skyrimworldmapcloudbank.nif", force);
    return blocks;
}

InterfaceSummary import_interface(const legacy::DataSource& data, const std::string& cache_dir, bool force) {
    InterfaceSummary summary;
    const std::filesystem::path cache(cache_dir);
    for (const auto& file : data.list("interface/", ".swf")) {
        const auto out = cache / (file.substr(0, file.size() - 4) + ".osu");
        if (!force && current_movie(out)) {
            ++summary.movies;
            continue;
        }
        auto bytes = data.read(file);
        if (!bytes) {
            continue;
        }
        try {
            MovieImport m = import_movie(*bytes);
            write_file(out, osu::write(m.movie));
            for (const auto& w : m.warnings) {
                log::debug("ui: {}: {}", file, w);
            }
            summary.warnings += m.warnings.size();
            ++summary.movies;
        } catch (const std::exception& e) {
            log::warn("ui: {}: {}", file, e.what());
            ++summary.movies_failed;
        }
    }
    for (const auto& file : data.list("interface/", ".txt")) {
        const std::string name = std::filesystem::path(file).filename().string();
        auto bytes = data.read(file);
        if (!bytes) {
            continue;
        }
        const auto out = cache / (file.substr(0, file.size() - 4) + ".osu");
        if (name.starts_with("translate_")) {
            write_file(out, osu::write(import_translations(*bytes)));
            ++summary.tables;
        } else if (name.starts_with("fontconfig")) {
            write_file(out, osu::write(import_font_config(*bytes)));
            ++summary.tables;
        } else if (name == "credits.txt" || name == "credits_plru.txt") {
            // The credits are lines of HTML the Credits Menu appends as they are (docs/formats/osu.md).
            write_file(cache / file, *bytes);
            ++summary.tables;
        }
    }
    write_file(cache / "interface" / "controls.osu", osu::write(import_controls(data, "english")));
    ++summary.tables;
    for (const std::string_view model : k_scene_models) {
        if (convert_scene_model(data, cache_dir, model, force)) {
            ++summary.models;
        }
    }
    summary.models += convert_world_map(data, cache_dir, force);
    log::info("ui: {} movies ({} failed, {} warnings), {} tables, {} models", summary.movies, summary.movies_failed,
              summary.warnings, summary.tables, summary.models);
    return summary;
}

} // namespace spud::importer
