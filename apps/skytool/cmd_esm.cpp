#include "commands.hpp"

#include "core/error.hpp"
#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "formats/osw.hpp"
#include "importer/convert/world_import.hpp"
#include "importer/legacy/esm.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace skytool {

namespace {

using spud::legacy::EsmDatabase;
using spud::legacy::Record;
using spud::legacy::record_type;
using spud::legacy::RecordEntry;
using spud::legacy::type_name;

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// A short human-readable rendering of a subrecord: its string if it looks like one, else hex.
std::string preview(std::span<const std::byte> data) {
    size_t n = data.size();
    bool printable = n >= 2 && static_cast<char>(data[n - 1]) == '\0';
    for (size_t i = 0; printable && i + 1 < n; ++i) {
        auto c = static_cast<unsigned char>(data[i]);
        printable = c >= 0x20 && c < 0x7F;
    }
    if (printable) {
        return std::format("\"{}\"", std::string_view(reinterpret_cast<const char*>(data.data()), n - 1));
    }
    std::string out;
    for (size_t i = 0; i < std::min<size_t>(n, 24); ++i) {
        out += std::format("{:02x}", static_cast<unsigned>(data[i]));
        if (i % 4 == 3) {
            out += ' ';
        }
    }
    if (n > 24) {
        out += "...";
    }
    if (n == 4) {
        float f;
        std::memcpy(&f, data.data(), 4);
        uint32_t u;
        std::memcpy(&u, data.data(), 4);
        out += std::format(" (u32 {} / f32 {})", u, f);
    }
    return out;
}

void print_header(const EsmDatabase& db, const RecordEntry& e) {
    std::printf("%s %08X  flags %08X  size %u  group %u  parent %08X  [%s]\n", type_name(e.type).c_str(),
                e.form_id, e.flags, e.data_size, static_cast<unsigned>(e.group), e.parent,
                db.plugins()[e.plugin].filename.c_str());
}

void print_subrecords(const Record& r) {
    for (const auto& s : r.subrecords()) {
        std::printf("    %s %6zu  %s\n", type_name(s.type).c_str(), s.data.size(), preview(s.data).c_str());
    }
}

template <typename T>
bool same_bytes(const std::vector<T>& a, const std::vector<T>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

bool same_world(const spud::osw::World& a, const spud::osw::World& b) {
    bool textures = a.land_textures.size() == b.land_textures.size();
    for (size_t i = 0; textures && i < a.land_textures.size(); ++i) {
        const auto& x = a.land_textures[i];
        const auto& y = b.land_textures[i];
        textures = x.form_id == y.form_id && x.diffuse == y.diffuse && x.normal == y.normal &&
                   x.specular_exponent == y.specular_exponent;
    }
    return textures && a.edid == b.edid && a.name == b.name && a.parent_edid == b.parent_edid &&
           a.form_id == b.form_id && a.flags == b.flags && a.cell_min[0] == b.cell_min[0] &&
           a.cell_min[1] == b.cell_min[1] && a.cell_max[0] == b.cell_max[0] &&
           a.cell_max[1] == b.cell_max[1] && a.cell_size == b.cell_size &&
           a.default_land_height == b.default_land_height &&
           a.default_water_height == b.default_water_height &&
           a.default_land_texture == b.default_land_texture && a.meshes == b.meshes &&
           same_bytes(a.cells, b.cells) && same_bytes(a.lands, b.lands) &&
           same_bytes(a.land_layers, b.land_layers) && same_bytes(a.refs, b.refs);
}

uint32_t parse_form_id(std::string_view s) {
    if (s.starts_with("0x") || s.starts_with("0X")) {
        s.remove_prefix(2);
    }
    uint32_t id = 0;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), id, 16);
    if (ec != std::errc() || end != s.data() + s.size()) {
        throw spud::Error(std::format("not a hexadecimal FormID: '{}'", s));
    }
    return id;
}

int esm_plugins(const EsmDatabase& db) {
    std::printf("%-7s %-48s %-5s %8s %8s %s\n", "index", "plugin", "flags", "records", "groups", "masters");
    for (const auto& p : db.plugins()) {
        std::string flags;
        flags += p.master ? 'M' : '-';
        flags += p.light ? 'L' : '-';
        flags += p.localized ? 'S' : '-';
        std::string masters;
        for (const auto& m : p.masters) {
            masters += (masters.empty() ? "" : ", ") + m;
        }
        std::printf("%-7s %-48s %-5s %8u %8u %s\n", p.index_string().c_str(), p.filename.c_str(),
                    flags.c_str(), p.entry_count, p.group_count, masters.c_str());
    }
    std::printf("%zu plugins, %zu records, %zu unique forms\n", db.plugins().size(), db.entries().size(),
                db.winner_count());
    return 0;
}

int esm_dump(const EsmDatabase& db, Args args, const std::string& plugin_name) {
    auto plugin = db.find_plugin(plugin_name);
    if (!plugin) {
        spud::log::error("{} is not in the load order", plugin_name);
        return 1;
    }
    auto type = option(args, "--type");
    auto filter = type ? spud::legacy::parse_type(*type) : 0;
    auto limit = std::stoul(option(args, "--limit").value_or("100"));
    size_t shown = 0;
    std::map<std::string, size_t> counts;
    for (const auto& e : db.plugin_entries(*plugin)) {
        ++counts[type_name(e.type)];
        if ((filter && e.type != filter) || shown >= limit) {
            continue;
        }
        print_header(db, e);
        try {
            print_subrecords(db.read(e));
        } catch (const std::exception& ex) {
            spud::log::error("{}", ex.what());
        }
        ++shown;
    }
    std::printf("record types:");
    for (const auto& [t, n] : counts) {
        std::printf(" %s:%zu", t.c_str(), n);
    }
    std::printf("\n");
    return 0;
}

int esm_record(const EsmDatabase& db, Args args, const std::string& id) {
    auto form_id = parse_form_id(id);
    auto versions = db.versions(form_id);
    if (versions.empty()) {
        spud::log::error("{:08X} is not defined by any plugin", form_id);
        return 1;
    }
    for (size_t i = 0; i < versions.size(); ++i) {
        std::printf("%s ", i + 1 == versions.size() ? "winner:" : "       ");
        print_header(db, *versions[i]);
    }
    const auto& winner = *versions.back();
    if (winner.deleted()) {
        std::printf("(deleted by %s)\n", db.plugins()[winner.plugin].filename.c_str());
    }
    if (has_flag(args, "--all")) {
        for (const auto* v : versions) {
            std::printf("[%s]\n", db.plugins()[v->plugin].filename.c_str());
            print_subrecords(db.read(*v));
        }
    } else {
        print_subrecords(db.read(winner));
    }
    auto kids = db.children(form_id);
    if (!kids.empty()) {
        std::map<std::string, size_t> counts;
        for (auto c : kids) {
            ++counts[type_name(c->type)];
        }
        std::printf("children:");
        for (const auto& [t, n] : counts) {
            std::printf(" %s:%zu", t.c_str(), n);
        }
        std::printf("\n");
        auto list_type = option(args, "--children");
        for (auto c : kids) {
            if (list_type && (*list_type == "all" || spud::legacy::parse_type(*list_type) == c->type)) {
                std::printf("  ");
                print_header(db, *c);
            }
        }
    }
    return 0;
}

int esm_worlds(const EsmDatabase& db) {
    static constexpr auto k_cell = record_type("CELL");
    static constexpr auto k_land = record_type("LAND");
    static constexpr auto k_navm = record_type("NAVM");
    std::printf("%-8s %-40s %7s %7s %8s %-20s %s\n", "formid", "edid", "cells", "lands", "refs", "parent",
                "plugin");
    for (auto w : db.of_type(record_type("WRLD"))) {
        auto rec = db.read(*w);
        size_t cells = 0;
        size_t lands = 0;
        size_t refs = 0;
        for (auto c : db.children(w->form_id)) {
            if (c->type != k_cell) {
                continue;
            }
            if (c->group != spud::legacy::group_world_children) {
                ++cells;
            }
            for (auto child : db.children(c->form_id)) {
                if (child->type == k_land) {
                    ++lands;
                } else if (child->type != k_navm) {
                    ++refs;
                }
            }
        }
        std::string parent;
        if (auto p = rec.form(record_type("WNAM"))) {
            if (auto pr = db.get(p)) {
                parent = pr->edid();
            } else {
                parent = std::format("{:08X}", p);
            }
        }
        std::printf("%08X %-40s %7zu %7zu %8zu %-20s %s\n", w->form_id, std::string(rec.edid()).c_str(),
                    cells, lands, refs, parent.c_str(), db.plugins()[w->plugin].filename.c_str());
    }
    std::printf("%zu worldspaces with exterior cells\n", spud::importer::list_worldspaces(db).size());
    return 0;
}

int esm_world(const EsmDatabase& db, Args args, const std::string& edid) {
    auto t0 = std::chrono::steady_clock::now();
    auto result = spud::importer::import_world(db, edid);
    auto import_ms = ms_since(t0);
    const auto& w = result.world;

    size_t with_land = 0;
    size_t with_water = 0;
    size_t synthetic = 0;
    for (const auto& c : w.cells) {
        with_land += (c.flags & spud::osw::cell_has_land) ? 1 : 0;
        with_water += (c.flags & spud::osw::cell_has_water) ? 1 : 0;
        synthetic += c.form_id == 0 ? 1 : 0;
    }
    size_t disabled = 0;
    size_t persistent = 0;
    for (const auto& r : w.refs) {
        disabled += (r.flags & spud::osw::ref_initially_disabled) ? 1 : 0;
        persistent += (r.flags & spud::osw::ref_persistent) ? 1 : 0;
    }
    float h_min = 1e30f;
    float h_max = -1e30f;
    for (const auto& l : w.lands) {
        for (float h : l.heights) {
            h_min = std::min(h_min, h);
            h_max = std::max(h_max, h);
        }
    }
    std::printf("world %s (%08X) name '%s' parent '%s' flags %u\n", w.edid.c_str(), w.form_id, w.name.c_str(),
                w.parent_edid.c_str(), w.flags);
    std::printf("  grid          [%d, %d] .. [%d, %d], cell size %.3f m\n", w.cell_min[0], w.cell_min[1],
                w.cell_max[0], w.cell_max[1], w.cell_size);
    std::printf("  defaults      land %.2f m, water %.2f m, texture %s\n", w.default_land_height,
                w.default_water_height, w.default_land_texture.c_str());
    std::printf("  cells         %zu (%zu with land, %zu with water, %zu synthetic)\n", w.cells.size(),
                with_land, with_water, synthetic);
    std::printf("  lands         %zu, heights %.1f .. %.1f m\n", w.lands.size(),
                w.lands.empty() ? 0.0f : h_min, w.lands.empty() ? 0.0f : h_max);
    std::printf("  land layers   %zu, land textures %zu, texture files %zu\n", w.land_layers.size(),
                w.land_textures.size(), result.textures.size());
    std::printf("  refs          %zu (%zu initially disabled, %zu persistent)\n", w.refs.size(), disabled,
                persistent);
    std::printf("  unique meshes %zu\n", w.meshes.size());
    std::printf("  skipped refs  %zu\n", result.skipped_total());
    for (const auto& [reason, n] : result.skipped) {
        std::printf("    %8zu  %s\n", n, reason.c_str());
    }
    std::printf("  regions       %zu merged worlds\n", w.regions.size());
    for (const auto& r : w.regions) {
        std::printf("    %08X %s\n", r.form_id, r.edid.c_str());
    }
    std::printf("  doors         %zu load doors, %zu rooms, %zu portals, %zu occluders\n", w.doors.size(), w.rooms.size(),
                w.portals.size(), w.occluders.size());
    if (!result.merge_kept.empty()) {
        std::vector<std::pair<size_t, uint32_t>> kept;
        for (const auto& [base, n] : result.merge_kept) {
            kept.push_back({n, base});
        }
        std::sort(kept.rbegin(), kept.rend());
        std::printf("  parent refs kept in shared cells, visible-when-distant bases\n");
        for (const auto& [n, base] : kept) {
            auto rec = db.get(base);
            if (rec && (rec->flags() & 0x8000)) {
                std::printf("    %6zu  %08X %s %s\n", n, base, spud::legacy::type_name(rec->type()).c_str(),
                            std::string(rec->edid()).c_str());
            }
        }
    }
    std::printf("  import        %.0f ms\n", import_ms);

    if (auto out = option(args, "--out")) {
        auto t1 = std::chrono::steady_clock::now();
        auto bytes = spud::osw::write(w);
        spud::write_file(*out, bytes);
        bool same = same_world(spud::osw::read(spud::read_file(*out)), w);
        std::printf("  wrote         %s (%.1f MB, %.0f ms), round trip %s\n", out->c_str(),
                    static_cast<double>(bytes.size()) / (1024.0 * 1024.0), ms_since(t1),
                    same ? "ok" : "MISMATCH");
        if (!same) {
            return 1;
        }
    }
    return 0;
}

} // namespace

int cmd_esm(Args args) {
    auto pos = positionals(args, {"--type", "--limit", "--out", "--children"});
    if (pos.empty()) {
        std::puts("esm plugins                               load order, resolved indices, record counts\n"
                  "esm dump <plugin> [--type XXXX] [--limit N] records of one plugin with their subrecords\n"
                  "esm record <formid-hex> [--children XXXX|all] [--all] winning record (--all: every version), children\n"
                  "esm worlds                                worldspaces with cell / land / ref counts\n"
                  "esm world <edid> [--out <file.osw>]       import a worldspace and print statistics");
        return 1;
    }
    const auto& sub = pos[0];
    auto t0 = std::chrono::steady_clock::now();
    EsmDatabase db(open_install());
    spud::log::info("load order indexed in {:.0f} ms", ms_since(t0));

    if (sub == "plugins") {
        return esm_plugins(db);
    }
    if (sub == "dump" && pos.size() >= 2) {
        return esm_dump(db, args, pos[1]);
    }
    if (sub == "record" && pos.size() >= 2) {
        return esm_record(db, args, pos[1]);
    }
    if (sub == "worlds") {
        return esm_worlds(db);
    }
    if (sub == "world" && pos.size() >= 2) {
        return esm_world(db, args, pos[1]);
    }
    spud::log::error("unknown esm subcommand");
    return 1;
}

} // namespace skytool
