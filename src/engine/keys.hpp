#pragma once

// Skyrim's key bindings (controlmap.txt, imported into rules/game.osr) use DirectInput scan codes; SDL reports
// USB scancodes. This maps one to the other.

#include <SDL3/SDL_scancode.h>

#include <cstdint>

namespace spud::engine {

// SDL_SCANCODE_UNKNOWN for codes without a key (0xFF = unmapped).
SDL_Scancode scancode_from_dik(uint16_t dik);

} // namespace spud::engine
