#include "importer/convert/ui_worlds.hpp"

#include "importer/legacy/esm.hpp"

#include "core/file.hpp"
#include "core/log.hpp"
#include "formats/container.hpp"

#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>

namespace spud::importer {

osu::Strings import_worldspace_maps(const legacy::EsmDatabase& db) {
    // WRLD DATA (uint8) is the worldspace's flag byte (the game ->). A cell's
    // attach runs a functor over its references: one whose base is NorthMarker (STAT 0x00000003,
    // the hard-coded form of) sets an interior's north rotation, and an exterior worldspace's
    // when its flag 0x40 is set, to minus the marker's z angle
    // (REFR DATA: position, then x, y, z angles in radians).
    const legacy::RecordType wrld = legacy::record_type("WRLD"), cell = legacy::record_type("CELL");
    const legacy::RecordType data = legacy::record_type("DATA"), name = legacy::record_type("NAME");
    std::map<std::string, std::string> keys;
    size_t with_north = 0;
    for (const legacy::RecordEntry* e : db.of_type(wrld)) {
        const legacy::Record w = db.read(*e);
        std::string edid(w.edid());
        for (char& c : edid) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const legacy::Subrecord* d = w.find(data);
        const uint32_t flags = d && !d->data.empty() ? std::to_integer<uint32_t>(d->data[0]) : 0u;
        keys[edid + ".flags"] = std::to_string(flags);
        if (!(flags & 0x40u)) {
            continue;
        }
        bool found = false;
        for (const legacy::RecordEntry* c : db.children(e->form_id)) {
            if (c->type != cell || found) {
                continue;
            }
            for (const legacy::RecordEntry* r : db.children(c->form_id)) {
                if (r->type != legacy::record_type("REFR")) {
                    continue;
                }
                const legacy::Record ref = db.read(*r);
                if (ref.form(name) != 0x00000003u) {
                    continue;
                }
                const legacy::Subrecord* pos = ref.find(data);
                if (!pos || pos->data.size() < 24) {
                    continue;
                }
                float angles[3];
                std::memcpy(angles, pos->data.data() + 12, sizeof(angles));
                keys[edid + ".north"] = std::format("{:.9g}", -angles[2]);
                keys[edid + ".north_marker"] = std::format("{:08X}", ref.form_id());
                log::info("ui worlds: {} north {:.6f} rad (NorthMarker {:08X})", edid, -angles[2],
                          ref.form_id());
                found = true;
                ++with_north;
                break;
            }
        }
    }
    container::StringTableBuilder strings;
    osu::Strings out;
    for (const auto& [k, v] : keys) {
        out.entries.push_back({strings.add(k), strings.add(v)});
    }
    const auto bytes = strings.bytes();
    out.strings.resize(bytes.size());
    std::memcpy(out.strings.data(), bytes.data(), bytes.size());
    log::info("ui worlds: {} worldspaces, {} with a north marker", db.of_type(wrld).size(), with_north);
    return out;
}

void write_worldspace_maps(const legacy::EsmDatabase& db, const std::string& cache_dir) {
    write_file(std::filesystem::path(cache_dir) / "interface" / "worldspaces.osu",
               osu::write(import_worldspace_maps(db)));
}

} // namespace spud::importer
