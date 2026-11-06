#include "importer/convert/world_tables.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "importer/convert/ui_strings.hpp"
#include "importer/convert/world_import.hpp"
#include "importer/legacy/data_source.hpp"
#include "importer/legacy/esm.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace spud::importer {

namespace {

using legacy::record_type;

constexpr uint32_t k_map_marker_static = 0x00000010; // MapMarker

template <class T>
T read_at(const legacy::Subrecord* s, size_t offset, T fallback = {}) {
    if (!s || offset + sizeof(T) > s->data.size()) {
        return fallback;
    }
    T v;
    std::memcpy(&v, s->data.data() + offset, sizeof(T));
    return v;
}

} // namespace

osg::World import_world_tables(const legacy::EsmDatabase& db, const legacy::DataSource& data) {
    const GameStrings strings(db, data, "english");
    osg::World t;
    const auto full = record_type("FULL");
    const auto d = record_type("DATA");
    auto name_of = [&](const legacy::Record& r) {
        const std::string name = strings.text(r, full);
        return name.empty() ? osg::k_none : t.add_string(name);
    };

    // One pass over the placed references: map markers (the MapMarker static with XMRK) and load doors (XTEL).
    const auto xmrk = record_type("XMRK"), fnam = record_type("FNAM"), tnam = record_type("TNAM");
    const auto xesp = record_type("XESP"), xrds = record_type("XRDS"), xlkr = record_type("XLKR");
    const auto xtel = record_type("XTEL"), name_sig = record_type("NAME");
    auto world_of = [&](uint32_t cell) {
        const legacy::RecordEntry* c = db.find(cell);
        return c ? c->parent : 0u;
    };
    for (const legacy::RecordEntry* e : db.of_type(record_type("REFR"))) {
        const legacy::Record r = db.read(*e);
        const legacy::Subrecord* pos = r.find(d);
        if (const legacy::Subrecord* tel = r.find(xtel)) {
            osg::Door o{};
            o.form_id = r.form_id();
            o.cell = e->parent;
            o.world = world_of(e->parent);
            o.destination = r.form(xtel);
            for (size_t i = 0; i < 3; ++i) {
                o.position[i] = read_at<float>(pos, i * 4);
                o.exit_position[i] = read_at<float>(tel, 4 + i * 4);
            }
            if (r.flags() & 0x800) {
                o.flags |= osg::door_disabled;
            }
            if (auto base = db.get(r.form(name_sig)); base && (read_at<uint8_t>(base->find(fnam), 0) & 0x8)) {
                o.flags |= osg::door_minimal_use;
            }
            t.doors.push_back(o);
        }
        if (!r.has(xmrk) || r.form(name_sig) != k_map_marker_static) {
            continue;
        }
        osg::MapMarker m{};
        m.form_id = r.form_id();
        m.cell = e->parent;
        m.world = world_of(e->parent);
        m.name = name_of(r);
        for (size_t i = 0; i < 3; ++i) {
            m.position[i] = read_at<float>(pos, i * 4);
            m.arrival_position[i] = m.position[i];
            m.arrival_rotation[i] = read_at<float>(pos, 12 + i * 4);
        }
        m.rotation_z = m.arrival_rotation[2];
        m.radius = read_at<float>(r.find(xrds), 0);
        m.flags = read_at<uint8_t>(r.find(fnam), 0);
        m.type = read_at<uint16_t>(r.find(tnam), 0);
        m.record_flags = r.flags();
        m.enable_parent = r.form(xesp);
        m.enable_flags = read_at<uint8_t>(r.find(xesp), 4);
        // XESP flag 0x01: the opposite of the parent.
        m.start_disabled = (r.flags() & 0x800) != 0;
        if (m.enable_parent) {
            const legacy::RecordEntry* parent = db.find(m.enable_parent);
            const bool off = parent && (parent->flags & 0x800);
            m.start_disabled = m.start_disabled || off != ((m.enable_flags & 0x1) != 0);
        }
        m.arrival_world = m.world;
        m.arrival_cell = m.cell;
        // The game with no keyword: the XLKR whose keyword is 0.
        for (const legacy::Subrecord& link : r.subrecords()) {
            if (link.type != xlkr || link.data.size() < 8 || read_at<uint32_t>(&link, 0) != 0) {
                continue;
            }
            const uint32_t to = r.resolve(read_at<uint32_t>(&link, 4));
            const legacy::RecordEntry* te = db.find(to);
            const auto target = db.get(to);
            if (te && target) {
                const legacy::Subrecord* tp = target->find(d);
                m.arrival_cell = te->parent;
                m.arrival_world = world_of(te->parent);
                for (size_t i = 0; i < 3; ++i) {
                    m.arrival_position[i] = read_at<float>(tp, i * 4);
                    m.arrival_rotation[i] = read_at<float>(tp, 12 + i * 4);
                }
            }
            break;
        }
        t.markers.push_back(m);
    }

    const auto xezn = record_type("XEZN"), xlcn = record_type("XLCN");
    const MergedWorlds merged = merged_worlds(db);
    for (const legacy::RecordEntry* e : db.of_type(record_type("WRLD"))) {
        const legacy::Record r = db.read(*e);
        osg::Worldspace w{};
        w.form_id = r.form_id();
        w.edid = t.add_string(r.edid());
        w.parent = r.form(record_type("WNAM"));
        w.parent_flags = read_at<uint16_t>(r.find(record_type("PNAM")), 0);
        w.flags = read_at<uint8_t>(r.find(d), 0);
        const legacy::Subrecord* map = r.find(record_type("MNAM"));
        w.usable_size[0] = read_at<int32_t>(map, 0);
        w.usable_size[1] = read_at<int32_t>(map, 4);
        w.nw_cell[0] = read_at<int16_t>(map, 8);
        w.nw_cell[1] = read_at<int16_t>(map, 10);
        w.se_cell[0] = read_at<int16_t>(map, 12);
        w.se_cell[1] = read_at<int16_t>(map, 14);
        w.camera_min_height = read_at<float>(map, 16);
        w.camera_max_height = read_at<float>(map, 20);
        w.camera_initial_pitch = read_at<float>(map, 24);
        const legacy::Subrecord* offset = r.find(record_type("ONAM"));
        w.map_scale = read_at<float>(offset, 0, 1.0f);
        for (size_t i = 0; i < 3; ++i) {
            w.map_offset[i] = read_at<float>(offset, 4 + i * 4);
        }
        const legacy::Subrecord *lo = r.find(record_type("NAM0")), *hi = r.find(record_type("NAM9"));
        for (size_t i = 0; i < 2; ++i) {
            w.bounds_min[i] = read_at<float>(lo, i * 4);
            w.bounds_max[i] = read_at<float>(hi, i * 4);
        }
        w.encounter_zone = r.form(xezn);
        w.location = r.form(xlcn);
        const auto into = merged.find(w.form_id);
        w.space = t.add_string(path::to_lower(into != merged.end() ? std::string_view(into->second) : r.edid()));
        t.world_names.push_back(name_of(r));
        t.worldspaces.push_back(w);
    }

    for (const legacy::RecordEntry* e : db.of_type(record_type("ECZN"))) {
        const legacy::Record r = db.read(*e);
        const legacy::Subrecord* z = r.find(d);
        osg::EncounterZone o{};
        o.form_id = r.form_id();
        o.owner = r.form(d, 0);
        o.location = r.form(d, 4);
        o.rank = read_at<int8_t>(z, 8);
        o.min_level = read_at<int8_t>(z, 9);
        o.flags = read_at<uint8_t>(z, 10);
        o.max_level = read_at<int8_t>(z, 11);
        t.zones.push_back(o);
    }

    const auto kwda = record_type("KWDA");
    for (const legacy::RecordEntry* e : db.of_type(record_type("LCTN"))) {
        const legacy::Record r = db.read(*e);
        osg::Location l{};
        l.form_id = r.form_id();
        l.name = name_of(r);
        l.parent = r.form(record_type("PNAM"));
        l.marker = r.form(record_type("MNAM"));
        l.radius = read_at<float>(r.find(record_type("RNAM")), 0);
        l.first_keyword = static_cast<uint32_t>(t.keywords.size());
        if (const legacy::Subrecord* k = r.find(kwda)) {
            for (size_t at = 0; at + 4 <= k->data.size(); at += 4) {
                t.keywords.push_back(r.form(kwda, at));
            }
        }
        l.keyword_count = static_cast<uint32_t>(t.keywords.size()) - l.first_keyword;
        t.locations.push_back(l);
        // LCSR / ACSR entries: reference type, reference, world or cell, grid: 16 bytes.
        std::vector<uint32_t> removed;
        if (const legacy::Subrecord* gone = r.find(record_type("RCSR"))) {
            for (size_t at = 0; at + 4 <= gone->data.size(); at += 4) {
                removed.push_back(r.form(record_type("RCSR"), at));
            }
        }
        for (const auto sig : {record_type("LCSR"), record_type("ACSR")}) {
            const legacy::Subrecord* list = r.find(sig);
            for (size_t at = 0; list && at + 16 <= list->data.size(); at += 16) {
                const osg::LocationRef ref{l.form_id, r.form(sig, at), r.form(sig, at + 4)};
                if (std::find(removed.begin(), removed.end(), ref.ref) == removed.end()) {
                    t.location_refs.push_back(ref);
                }
            }
        }
    }

    const auto xclc = record_type("XCLC");
    for (const legacy::RecordEntry* e : db.of_type(record_type("CELL"))) {
        const legacy::Record r = db.read(*e);
        osg::Cell c{};
        c.encounter_zone = r.form(xezn);
        c.location = r.form(xlcn);
        const legacy::Subrecord* flags = r.find(d);
        c.flags = flags && flags->data.size() >= 2 ? read_at<uint16_t>(flags, 0) : read_at<uint8_t>(flags, 0);
        if (!c.encounter_zone && !c.location && !c.flags) {
            continue;
        }
        c.form_id = r.form_id();
        c.world = e->parent;
        t.cell_names.push_back(name_of(r));
        t.cell_edids.push_back(t.add_string(r.edid()));
        c.x = static_cast<int16_t>(read_at<int32_t>(r.find(xclc), 0));
        c.y = static_cast<int16_t>(read_at<int32_t>(r.find(xclc), 4));
        t.cells.push_back(c);
    }
    // Regions with a map name: the areas (RPLD, x and y pairs) under it.
    const auto rpld = record_type("RPLD"), rdmp = record_type("RDMP");
    for (const legacy::RecordEntry* e : db.of_type(record_type("REGN"))) {
        const legacy::Record r = db.read(*e);
        const std::string name = strings.text(r, rdmp);
        if (name.empty()) {
            continue;
        }
        const uint32_t label = t.add_string(name);
        const uint32_t world = r.form(record_type("WNAM"));
        for (const legacy::Subrecord& area : r.subrecords()) {
            if (area.type != rpld) {
                continue;
            }
            osg::MapRegion m{world, label, uint32_t(t.region_points.size()), uint32_t(area.data.size() / 8)};
            for (size_t at = 0; at + 4 <= area.data.size(); at += 4) {
                t.region_points.push_back(read_at<float>(&area, at));
            }
            t.map_regions.push_back(m);
        }
    }
    return t;
}

void write_world_tables(const legacy::EsmDatabase& db, const legacy::DataSource& data, const std::string& cache_dir) {
    const osg::World t = import_world_tables(db, data);
    write_file(std::filesystem::path(cache_dir) / "rules" / "world.osg", osg::write(t));
    log::info("world tables: {} map markers, {} worldspaces, {} encounter zones, {} locations, {} cells, {} doors",
              t.markers.size(), t.worldspaces.size(), t.zones.size(), t.locations.size(), t.cells.size(), t.doors.size());
}

} // namespace spud::importer
