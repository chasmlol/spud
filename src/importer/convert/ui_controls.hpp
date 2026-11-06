#pragma once

// The game's key names and control map for the menus (docs/formats/osu.md, kind UICT).

#include "formats/osu.hpp"

#include <string_view>

namespace spud::legacy {
class DataSource;
}

namespace spud::importer {

// interface/controls/pc/{keyboard_<language>,mouse,gamepad,controlmap}.txt -> key names, codes and bindings.
osu::Controls import_controls(const legacy::DataSource& data, std::string_view language);

} // namespace spud::importer
