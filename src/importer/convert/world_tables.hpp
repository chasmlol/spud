#pragma once

// What the world simulation needs of the load order (docs/formats/osg.md): <cache>/rules/world.osg.

#include "formats/osg.hpp"

#include <string>

namespace spud::legacy {
class DataSource;
class EsmDatabase;
} // namespace spud::legacy

namespace spud::importer {

osg::World import_world_tables(const legacy::EsmDatabase& db, const legacy::DataSource& data);
void write_world_tables(const legacy::EsmDatabase& db, const legacy::DataSource& data, const std::string& cache_dir);

} // namespace spud::importer
