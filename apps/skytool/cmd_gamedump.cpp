// `skytool records gamedump`: the facts the game records for each form, taken
// from the forms database through the typed accessors, in the game's JSON format, so the two can be diffed

#include "commands.hpp"

#include "core/error.hpp"
#include "formats/forms.hpp"
#include "formats/forms_typed.hpp"

#include <cstdio>
#include <format>
#include <string>

namespace skytool {

namespace {

using namespace spud;

void json_string(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

std::string id(uint32_t v) {
    return std::format("\"{:08X}\"", v);
}

std::string num(double v) {
    return std::format("{:.9g}", v);
}

void id_list(std::string& out, const char* key, const std::vector<uint32_t>& ids) {
    out += std::format(",\"{}\":[", key);
    for (size_t i = 0; i < ids.size(); ++i) {
        out += (i ? "," : "") + id(ids[i]);
    }
    out += ']';
}

std::string dump(const forms::Database& db, const forms::FormEntry& e) {
    forms::Form f(db, e);
    std::string out = "{\"id\":" + id(e.form_id) + ",\"type\":\"" + forms::sig_name(e.type) + "\"";
    out += std::format(",\"flags\":{}", e.flags);
    out += ",\"files\":[";
    auto src = db.sources(e);
    for (size_t i = 0; i < src.size(); ++i) {
        if (i) {
            out += ',';
        }
        json_string(out, db.plugin_name(src[i]));
    }
    out += ']';
    if (src.size() > 1) {
        // Where each version sat, and whether its plugin is a master: what decides which versions the game reads.
        auto groups = db.source_groups(e);
        out += ",\"source_groups\":[";
        for (size_t i = 0; i < groups.size(); ++i) {
            out += std::format("{}{}", i ? "," : "", groups[i]);
        }
        out += "],\"source_masters\":[";
        for (size_t i = 0; i < src.size(); ++i) {
            out += std::format("{}{}", i ? "," : "",
                               (db.plugins()[src[i]].flags & forms::plugin_master) ? 1 : 0);
        }
        out += ']';
    }
    if (f.get("FULL")) {
        out += ",\"name\":";
        json_string(out, f.name());
    }
    if (f.get("DESC")) {
        out += ",\"desc\":";
        json_string(out, f.description());
    }
    if (auto v = forms::item_value(f)) {
        out += std::format(",\"value\":{}", *v);
    }
    if (auto w = forms::item_weight(f)) {
        out += ",\"weight\":" + num(*w);
    }
    if (f.get("KWDA") || f.get("KSIZ")) {
        id_list(out, "keywords", f.keywords());
    }
    if (f.get("CNTO") || f.get("COCT")) {
        out += ",\"items\":[";
        auto items = forms::container_items(f);
        for (size_t i = 0; i < items.size(); ++i) {
            out += std::format("{}[{},{}]", i ? "," : "", id(items[i].item), items[i].count);
        }
        out += ']';
    }
    if (auto l = forms::leveled_list(f)) {
        out += std::format(",\"chance_none\":{},\"leveled_flags\":{},\"leveled\":[", l->chance_none, l->flags);
        for (size_t i = 0; i < l->entries.size(); ++i) {
            const auto& x = l->entries[i];
            out += std::format("{}[{},{},{}]", i ? "," : "", id(x.form), x.level, x.count);
        }
        out += ']';
    }
    if (f.get("EFID")) {
        out += ",\"effects\":[";
        auto fx = forms::effects(f);
        for (size_t i = 0; i < fx.size(); ++i) {
            out += std::format("{}[{},{},{},{}]", i ? "," : "", id(fx[i].effect), num(fx[i].magnitude), fx[i].area,
                               fx[i].duration);
        }
        out += ']';
    }
    if (e.type == forms::sig("FLST")) {
        id_list(out, "forms", forms::form_list(f));
    }
    if (auto g = forms::global_value(f)) {
        out += ",\"global\":" + num(*g);
    }
    if (e.type == forms::sig("INFO")) {
        // Responses as the dialogue system reads them: a shared info's lines when this one names it (DNAM).
        const forms::FormEntry* source = &e;
        for (int depth = 0; depth < 8; ++depth) {
            auto shared = forms::shared_info(forms::Form(db, *source));
            if (!shared || !db.find(shared)) {
                break;
            }
            source = db.find(shared);
        }
        out += ",\"responses\":[";
        auto lines = forms::responses(forms::Form(db, *source));
        for (size_t i = 0; i < lines.size(); ++i) {
            out += i ? "," : "";
            json_string(out, lines[i].text);
        }
        out += ']';
    }
    if (auto n = forms::navmesh(f)) {
        // The same sums as the game, accumulated the same way.
        double vsum = 0.0;
        for (const auto& v : n->vertices) {
            vsum += double(v[0]) + double(v[1]) + double(v[2]);
        }
        uint64_t isum = 0;
        uint64_t tsum = 0; // every 16-bit word of the triangle, as the game stores it
        for (const auto& t : n->triangles) {
            isum += uint64_t(t.vertices[0]) + t.vertices[1] + t.vertices[2];
            tsum += uint64_t(t.vertices[0]) + t.vertices[1] + t.vertices[2] + uint16_t(t.edges[0]) +
                    uint16_t(t.edges[1]) + uint16_t(t.edges[2]) + t.flags + t.cover_flags;
        }
        uint64_t csum = 0;
        for (int16_t c : n->cover_triangles) {
            csum += uint16_t(c);
        }
        std::string grid = std::to_string(n->grid_divisor);
        for (float v : {n->grid_cell_size[0], n->grid_cell_size[1], n->grid_min[0], n->grid_min[1], n->grid_min[2],
                        n->grid_max[0], n->grid_max[1], n->grid_max[2]}) {
            grid += "," + num(v);
        }
        // Edge links (type, navmesh, triangle): the comparison checks the portal edges the game drops against them.
        std::string links;
        for (const auto& l : n->edge_links) {
            links += std::format("{}[{},{},{}]", links.empty() ? "" : ",", l.type, id(l.navmesh), l.triangle);
        }
        out += std::format(",\"navmesh\":{{\"vertices\":{},\"triangles\":{},\"doors\":{},\"vertex_sum\":{},"
                           "\"index_sum\":{},\"triangle_sum\":{},\"cover_triangles\":{},\"cover_sum\":{},"
                           "\"grid\":[{}],\"edge_links\":[{}]}}",
                           n->vertices.size(), n->triangles.size(), n->doors.size(), num(vsum), isum, tsum,
                           n->cover_triangles.size(), csum, grid, links);
    }
    if (auto t = f.get("TPLT")) {
        out += ",\"template\":" + id(t->kind == forms::Value::Kind::form ? t->form : 0);
        if (auto flags = f.get("ACBS.template_flags")) {
            out += std::format(",\"template_flags\":{}", flags->integer);
        }
    }
    if (e.type == forms::sig("ARMO")) {
        if (auto t = f.get("TNAM"); t && t->kind == forms::Value::Kind::form) {
            out += ",\"template\":" + id(t->form);
        }
    }
    if (auto ep = f.get("XESP")) {
        if (auto r = ep->member("reference"); r && r->kind == forms::Value::Kind::form) {
            out += ",\"enable_parent\":" + id(r->form);
            auto fl = ep->member("flags");
            out += std::format(",\"enable_parent_flags\":{}", fl ? fl->integer : 0);
        }
    }
    if (e.type == forms::sig("GMST")) {
        out += ",\"edid\":";
        json_string(out, f.editor_id());
        if (auto v = forms::setting_value(f)) {
            out += ",\"setting\":" + v->json();
        }
    }
    out += std::format(",\"group\":{}", e.group);
    auto p = forms::placement(f);
    auto base = f.get("NAME");
    if (p || (base && base->kind == forms::Value::Kind::form && e.group >= 8 && e.group <= 10)) {
        // A reference; one without DATA (a deleted record holds only its base) has no placement.
        out += ",\"cell\":" + id(e.parent);
        for (auto [sig, key] : {std::pair{"XLIB", "leveled_item_base"}, std::pair{"XSPC", "spawn_container"}}) {
            if (auto v = f.get(sig); v && v->kind == forms::Value::Kind::form) {
                out += std::format(",\"{}\":{}", key, id(v->form));
            }
        }
        out += ",\"base\":" + id(p ? p->base : base->form);
        if (p) {
            out += ",\"rot\":[" + num(p->rotation[0]) + "," + num(p->rotation[1]) + "," + num(p->rotation[2]) +
                   "]";
            out += ",\"pos\":[" + num(p->position[0]) + "," + num(p->position[1]) + "," + num(p->position[2]) +
                   "]";
        }
    }
    out += "}\n";
    return out;
}

} // namespace

int records_gamedump(const forms::Database& db, Args args) {
    FILE* out = stdout;
    if (auto o = option(args, "--out")) {
        out = std::fopen(o->c_str(), "wb");
        if (!out) {
            throw Error("cannot write " + *o);
        }
    }
    // --navmesh-raw FILE: every navmesh's triangles and cover list as the game holds them in memory, for the data
    // the game's raw dumps (per navmesh: u32 FormID, u32 triangles, u32 cover entries, 16-byte triangles, u16 cover).
    FILE* raw = nullptr;
    if (auto o = option(args, "--navmesh-raw")) {
        raw = std::fopen(o->c_str(), "wb");
        if (!raw) {
            throw Error("cannot write " + *o);
        }
    }
    size_t n = 0;
    for (const auto& e : db.forms()) {
        auto line = dump(db, e);
        std::fwrite(line.data(), 1, line.size(), out);
        ++n;
        if (raw && e.type == forms::sig("NAVM")) {
            forms::Form f(db, e);
            if (auto nav = forms::navmesh(f)) {
                uint32_t head[3] = {e.form_id, uint32_t(nav->triangles.size()), uint32_t(nav->cover_triangles.size())};
                std::fwrite(head, sizeof head, 1, raw);
                for (const auto& t : nav->triangles) {
                    uint16_t w[8] = {t.vertices[0], t.vertices[1], t.vertices[2], uint16_t(t.edges[0]),
                                     uint16_t(t.edges[1]), uint16_t(t.edges[2]), t.flags, t.cover_flags};
                    std::fwrite(w, sizeof w, 1, raw);
                }
                std::fwrite(nav->cover_triangles.data(), 2, nav->cover_triangles.size(), raw);
            }
        }
    }
    if (out != stdout) {
        std::fclose(out);
    }
    if (raw) {
        std::fclose(raw);
    }
    std::fprintf(stderr, "gamedump: %zu forms\n", n);
    return 0;
}

} // namespace skytool
