# Roadmap

Spud is early. Most systems below have a working first version, and many still have rough edges.

## In place

- **Importer:** BSA, ESM/ESP/ESL with load order and Mod Organizer 2 profiles, NIF, HKX, Papyrus, audio and interface movies, all converted into native formats.
- **Rendering:** the game's lighting model on Vulkan, sky and weather, water, shadows, particles, grass, generated LOD and the far view. No temporal anti-aliasing ([RENDERING.md](RENDERING.md)).
- **World:** streaming worldspaces, interiors reached through their doors, cities in the open world ([ARCHITECTURE.md](ARCHITECTURE.md#world-model-spaces-and-portals)).
- **Actors:** skeletons, skinning, faces and a behaviour-graph runtime.
- **Movement:** collision on Jolt, a character controller, first and third person ([MOVEMENT.md](MOVEMENT.md)).
- **The world running:** Papyrus VM, conditions, quests, dialogue, AI packages, combat, magic, items, crime and saves.
- **Interface:** the game's own menus, played from your data ([UI.md](UI.md)).
- **Audio:** sound, music and headphone spatial audio ([AUDIO.md](AUDIO.md)).

## Next

- Bug fixing and polish across all of the above.
- Mod support beyond what the importer already converts.

## Later

- A native plugin API.
- An editor.
- Linux and Steam Deck.
- Support for building your own games on the engine.
