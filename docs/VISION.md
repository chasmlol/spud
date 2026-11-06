# Spud: Vision

Spud is an open-source C++ engine for open-world RPGs, with two aims:

1. **Run Skyrim Special Edition from your own copy of the game,** with the same look and feel as the original. Spud never ships game content.
2. **Be a general engine.** The same engine, without the Skyrim rules, can be a base for new open-world RPGs: plugin and load-order modding, cells and worldspaces, data-driven actors, quests and dialogue.

## The target: the feel, not the bugs

Spud aims for the exact look and feel of the game, visually and in how it behaves, but small odd quirks don't need to carry over. Physics runs on Jolt rather than Havok, so bit-exact emulation is not a goal.

**Rule: drop a quirk unless shipped content depends on it.**

| Tier | What | Standard |
|---|---|---|
| 1. Must match | Anything game data relies on: Papyrus semantics (event order, latent calls, states), condition functions, quest stages and aliases, dialogue selection, AI package selection. | Exact. Divergence breaks quests, not just feel. Quirks that content relies on are kept. |
| 2. Must feel the same | Movement, camera, animation timing and blending, combat timing, lighting, weather, colour grading, audio mix and music. | Close to the real game, within a tolerance. |
| 3. Free to differ | Bugs, exploits and engine limits: physics breaking above 60 fps, objects launching on cell load, NPCs falling through floors, load stutter, memory caps. | Fix them. Popular ones may come back as options. |

Every value and behaviour that's meant to match the game should be checked, not guessed: a physics-library default or a "reasonable" number isn't good enough. A feature counts as verified only when it has been compared with the real game, not just tested inside Spud.

## Image clarity

An old-school engine with modern GPU-driven speed underneath: the picture is never smeared across frames. No TAA, no TAA-based upscaling, no frame generation and no ghosting. Individual effects such as water and fog may use temporal tricks internally. Anti-aliasing is MSAA, alpha-to-coverage and specular AA. See [RENDERING.md](RENDERING.md).

## Deliberately better than the original

Some of the original's limits came from 2011 hardware, not from design. Spud improves these on purpose:

- **LOD and draw distance.** Spud generates its own high-quality LOD from the full-detail data at import (see [ARCHITECTURE.md](ARCHITECTURE.md#lod)). The original LOD files are never used, and distant views aren't expected to match.
- **No load screens.** One seamless world: cities are part of the open world, and interiors open straight off it through their doors (see [ARCHITECTURE.md](ARCHITECTURE.md#world-model-spaces-and-portals)).

## Native formats only

The engine loads **only Spud's own formats**. The game's formats (ESM/ESP/ESL, BSA, NIF, HKX, PEX, xWMA/FUZ/LIP, SWF) are read by a separate **importer**, which converts your install and your mods, incrementally, into native assets on your machine. See [ARCHITECTURE.md](ARCHITECTURE.md).

Generic formats that aren't game-specific (DDS/BCn textures, WAV) are kept as they are.

## Modding

- Mods in their original formats are converted by the importer.
- Papyrus functions added by SKSE can be reimplemented natively. SKSE DLL plugins hook the original game and can't work, so Spud will provide its own native plugin API instead.

## License

MIT, see [LICENSE](../LICENSE).
