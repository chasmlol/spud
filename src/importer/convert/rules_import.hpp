#pragma once

// Load order -> rules/game.osr: game settings, movement types, race movement data, the player's body and
// camera dimensions (from the skeleton NIFs) and the default key bindings (controlmap.txt).

#include "formats/okc.hpp"
#include "formats/osr.hpp"
#include "importer/legacy/data_source.hpp"
#include "importer/legacy/esm.hpp"

#include <string>
#include <vector>

namespace spud::importer {

struct RulesImport {
    osr::Rules rules;
    std::vector<std::string> warnings;
};

// Every camera model a CAMS record names (docs/formats/okc.md).
spud::okc::Cameras import_cameras(const legacy::EsmDatabase& db, const legacy::DataSource& data,
                                  std::vector<std::string>& warnings);

RulesImport import_rules(const legacy::EsmDatabase& db, const legacy::DataSource& data);

// controlmap.txt -> key bindings (exposed for tests and tools).
std::vector<osr::KeyBinding> parse_control_map(std::string_view text);

} // namespace spud::importer
