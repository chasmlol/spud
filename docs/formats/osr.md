# .osr — rules (kind `RULE`, version 5)

Layout: [`src/formats/osr.hpp`](../../src/formats/osr.hpp). One file per import, `rules/game.osr`: the game settings and records that drive gameplay systems, taken from the whole load order (later plugins win). It is small (tens of KB) and rewritten on every import.

| Chunk | Contents |
|---|---|
| `SETS` | `{name, value f64}` per numeric game setting (GMST), sorted by name |
| `MOVT` | movement types: FormID, EditorID, behaviour state name, speeds, thresholds |
| `RACE` | races: FormID, EditorID, height and weight, mass, acceleration rates, size, default movement types, skeleton paths |
| `PLYR` | the player: NPC and race FormIDs, NPC height, sex, eye height, body bounds, third-person camera height (version 3) |
| `KEYS` | default key bindings: event, input context, two keyboard keys, mouse, gamepad |
| `COLL` | collision layers (version 2): `{index, edid, flags, reserved, collides u64}` per COLL record, sorted by index |
| `STAI` | stairs materials (version 4): sorted `u32` Havok material IDs |
| `SHAK` | the camera shake (version 5): `{x, y, z}` f32 euler angles every 1/120 s |
| `FURM` | furniture markers (version 5): `{form u32, index u32, offset 3 x f32, heading f32, animation u16, entry u16}`, by form then index |
| `STRS` | string table |

## Semantics

- **Game settings** keep Skyrim's values and units (`fJumpHeightMin` stays 76 units). Only numeric settings are stored (`f`, `i`, `u`, `b` prefixes); string settings are UI text. Settings the game defines in its executable and no plugin overrides are absent; the engine supplies those defaults itself and says where each came from.
- **Movement types** (MOVT `SPED`) are converted to **metres per second** for left, right, forward and back, each as walk and run, plus rotation in place (walk, run) and while moving, in radians per second. `INAM` thresholds: directional (radians), speed (m/s), rotation (rad/s); -1 where the record has none (FLT_MAX).
- **Races** keep their DATA values as authored. The acceleration and deceleration rates have no documented unit; the engine treats them as multipliers.
- **Player** dimensions are unscaled skeleton values in metres; multiply by the race height (by sex) and the NPC height:
  - `eye_height`: the `Camera1st [Cam1]` node of `meshes/actors/character/_1stperson/skeleton.nif` in its bind pose, above the feet.
  - `bound_centre`, `bound_half`: the `BBX` BSBound of the race's third-person skeleton.
  - `camera3_height` (version 3): the local translation z of the `Camera3rd [Cam3]` node of the race's third-person skeleton, the third-person camera's pivot height above the feet. The game uses it unscaled. Versions 1 and 2 have a 44-byte `PLYR` without it; readers take both.
- **Race bounds** (`RBND`, version 6), parallel to the races: each skeleton's BSBound "BBX", male and female, metres, unscaled: centre then half extents. What an actor's controller is built from.
- **Collision layers** are the game's runtime collision matrix: after loading the plugins, the game replaces each row of its built-in matrix with the layers its COLL record lists. `collides` has bit n set when the layer collides with layer n. `flags` keeps GNAM: bit 0 trigger volume, bit 1 sensor (contacts with sensor layers neither block nor support a character). Version 1 files have no `COLL` chunk; readers accept both.
- **Stairs materials** are the MATT records whose FNAM has bit 0 ("stair material"), as the material ID shapes carry: the game of the lower-cased MNAM (CRC-32 with the reflected polynomial 0xEDB88320 from 0, no final xor). A character contact on such a surface is always a step. Older files have no `STAI` chunk.
- **Camera shake** is `meshes/actors/character/camerashake.nif`'s one rotation track, sampled over its 3.33 s loop: radians about the camera's right, forward and up. Older files have no `SHAK` chunk and nothing shakes.
- **Furniture markers** are each FURN's model's `BSFurnitureMarkerNode`: where a user's root ends up in the furniture's space, in units, and its heading in radians. A marker's number is its place in the list; the record's active markers and entry points say which are used.
- **Keys** come from `interface/controls/pc/controlmap.txt`. Keyboard keys are DirectInput scan codes, mouse ids follow the file (0 left, 1 right, 8/9 wheel, 0x0A mouse move), 0xFF means unmapped. Contexts are numbered in file order, 0 = main gameplay. Chords (`0x1d`) keep their last key.
