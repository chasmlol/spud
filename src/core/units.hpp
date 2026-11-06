#pragma once

namespace spud::units {

// Skyrim measures in "units": 1 unit = 0.0142875 m (70 units ~= 1 m).
// spud works in metres; the importer converts.
inline constexpr float k_skyrim_unit_m = 0.0142875f;

// Exterior cells are 4096 units square.
inline constexpr float k_cell_size_units = 4096.0f;
inline constexpr float k_cell_size_m = k_cell_size_units * k_skyrim_unit_m;

inline constexpr float to_metres(float skyrim_units) { return skyrim_units * k_skyrim_unit_m; }
inline constexpr float to_skyrim_units(float metres) { return metres / k_skyrim_unit_m; }

} // namespace spud::units
