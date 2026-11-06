#pragma once

// What the Map Menu's local map needs of each worldspace: its flag
// byte (WRLD DATA) and, for a worldspace with flag 0x40, the north rotation its NorthMarker reference gives
// it. <cache>/interface/worldspaces.osu, a string table (docs/formats/osu.md): "<edid>.flags", "<edid>.north"
// (radians),
// "<edid>.north_marker" (the reference's form ID), EDIDs in lower case.

#include "formats/osu.hpp"

#include <string>

namespace spud::legacy {
class EsmDatabase;
} // namespace spud::legacy

namespace spud::importer {

osu::Strings import_worldspace_maps(const legacy::EsmDatabase& db);
void write_worldspace_maps(const legacy::EsmDatabase& db, const std::string& cache_dir);

} // namespace spud::importer
