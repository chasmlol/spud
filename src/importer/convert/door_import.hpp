#pragma once

// DOOR records -> rules/doors.oda: each door model's Open and Close sequences (NiControllerManager), sampled into
// rigid poses of its moving shapes, and the door's sounds., "Doors".

#include "formats/oda.hpp"
#include "importer/legacy/data_source.hpp"
#include "importer/legacy/esm.hpp"

namespace spud::importer {

oda::File import_doors(const legacy::EsmDatabase& db, const legacy::DataSource& data);

} // namespace spud::importer
