# Movement and collision

The player can walk the imported world: Skyrim's speeds, jump and eye height on a Jolt character controller, against the game's own collision. **N** switches between walking and the noclip camera at any time.

## Pieces

| Layer | What | Where |
|---|---|---|
| Importer | Havok collision in NIFs → `.osp` next to each `.osm` | `src/importer/convert/collision_import.cpp`, [formats/osp.md](formats/osp.md) |
| Importer | Game settings, movement types, races, player body and eye, key bindings → `rules/game.osr` | `src/importer/convert/rules_import.cpp`, [formats/osr.md](formats/osr.md) |
| Physics | Static collision streamed around the player; the character controller | `src/physics` (Jolt, private to that directory) |
| Engine | Movement rules, fixed tick, interpolation, controls, script commands | `src/engine/player.*`, `src/engine/app_player.cpp` |

### Collision streaming

`physics::World` loads collision on its own small radius around the player (64 m in, 112 m out), not the render radius: nothing farther away can touch a 2 m capsule. Per cell:

- **Terrain**: a Jolt heightfield from the `.osw` land heights (33 x 33 samples, block size 3, quantisation error under 2 mm).
- **References**: one fixed body each, from the mesh's `.osp` shapes and the reference's position, rotation and scale. Shapes are built once per mesh and shared. Initially disabled references don't collide.

Shapes are read and built on two loader threads. The simulation thread adds finished cells to the world and removes far ones within a time budget, and rebuilds the broad phase once streaming settles. The render thread never touches Jolt.

Only shapes on layers the character collides with are built (static, anim static, transparent, clutter, trees, props, terrain, ground, traps, invisible walls, stair helpers, ...). Triggers, water, weapons, projectiles, non-collidable and pick layers are kept in the `.osp` for later systems but not built.

### Determinism

- Body IDs are fixed by the cell and reference index (terrain of cell *i* is body *i*, reference *r* is `cells + r`). The controller orders contacts by body ID, so contacts resolve the same way however the area was streamed in.
- A tick only runs when every cell within 24 m of the player has its collision; until then the player waits and time stands still for it.
- Scripts hold the player between scripted moves, so a script gives the same ticks at any frame rate. Headless control sessions keep that hold; windowed sessions return to live controls when the script is done.
- Checked: the same movement script at 1280x720 and 2560x1440 writes byte-identical traces.

### Ticks without frame spikes

The simulation runs at 60 Hz on the player's own thread, **one tick ahead**: while the camera shows the interpolation between ticks *k-1* and *k*, tick *k+1* (collision streaming plus the character step) is computing. A late tick is never waited for unless frames are slower than the tick rate; the camera simply holds the last tick. Mouse look is applied every frame, outside the simulation, so turning has no added latency; movement input has one to two ticks (16-33 ms).

## Values and where they come from

Skyrim units are 0.0142875 m. All of these are imported or derived at load and logged at startup (`player: walk ... run ...`).

| Value | Source | Value used |
|---|---|---|
| Walk / run speeds by direction | MOVT `NPC_Default_MT` (`SPED`, the unarmed weapon-sheathed state): forward 80.1 / 370, back 71.93 / 205.25, left 80.09 / 370, right 79.75 / 370 units/s | 1.14 / 5.29 m/s forward |
| Sprint | MOVT `NPC_Sprinting_MT`, run column: forward 500, back 270.84 units/s, no strafe speeds | 7.14 m/s forward; sideways parts at run speed |
| Directions in between | Blended by angle between the two directional speeds | |
| Jump height | GMST `fJumpHeightMin` = 76 units | apex 1.086 m above take-off (measured 76.2 units) |
| Gravity in the air | Havok world gravity 9.81 m/s² (1 Havok unit = 1 m at Skyrim's 69.99124 units per Havok unit) x `fInAirFallingCharGravityMult` | 13.24 m/s² |
| `fInAirFallingCharGravityMult` | Not in any plugin: the game's default, **1.35**, from public documentation (UESP's game settings list). Used if a plugin ever sets it. | 1.35 |
| Eye height | `Camera1st [Cam1]` node of `meshes/actors/character/_1stperson/skeleton.nif`: 120.5 units above the feet in the bind pose, x the race height (`NordRace` male 1.03, the player NPC's race) x the NPC height (1.0) | 1.773 m |
| Capsule | `BBX` bound of the race's third-person skeleton: half extents 22 x 14 x 64 units. Height = the box, radius = the mean of the two horizontal half extents, both x 1.03 | r 0.265 m, h 1.884 m |
| Field of view (walking) | `fDefaultWorldFOV` = 80, horizontal at 16:9: 50.6° vertical, kept at any aspect (Hor+) | 50.6 degrees |
| Acceleration / deceleration | RACE `DATA` acceleration and deceleration rates (both 1.0 for Nords) divide our ramp times. The unit of the race values is undocumented. | **placeholders**: 0.25 s to full speed, 0.18 s to stop |
| Air control | Not found in the data | **placeholder** 15% of ground acceleration |
| Step height, slope limit | Not found in the data | **placeholders** 0.4 m, 50 degrees |
| Key bindings | `interface/controls/pc/controlmap.txt` (Skyrim - Interface.bsa), main gameplay context | W/S/A/D, Space jump, Alt sprint, Shift walk, Caps Lock always run, C auto-move |

The placeholders are tunable live (F1 panel, or `set player_accel_time` etc.).

## Script commands and tests

See [AUTOMATION.md](AUTOMATION.md): `mode walk|fly`, `player x y z yaw pitch`, `move <dir> <seconds> [walk|run|sprint] [jump]`, `jump`, `trace <file.csv>` / `trace off`; `state` reports `mode`, `player`, `movement` (the values in use) and `physics`.

Collision alignment was checked offline against the render meshes: over the 151 Whiterun architecture meshes with mesh collision, collision vertices sit a median 2 cm from the drawn surface (p75 6.5 cm, p90 15 cm). The rest is collision that has no drawn counterpart in the same file (merged building collision, blocking volumes). `skytool nif scan` reports files whose collision reaches well past their render bounds: about 2% of the 12,000 files with collision; the ones inspected (stairs, cave walls, Jorrvaskr) are authored that way.

## Known gaps

- **Movable clutter is static.** Bodies keep their Havok motion type in `.osp`, but nothing is simulated yet; plates and baskets are small obstacles the player steps over.
- **Swimming.** Water is not a medium yet: the player walks along the bottom.
- **Sneaking** (speeds are imported, `NPC_Sneaking_MT`) and its lower camera; weapon-drawn movement types; encumbrance and the speed actor value; stamina for sprinting.
- **Landing and falling**: no landing slowdown, no fall damage (`fJumpFallHeight*` are imported), no terminal velocity.
- **Animation-driven feel**: first-person mounted head bob.
- **Collision-only meshes** (collision without anything to draw) are not placed: the world format only keeps references with a render mesh. One reference in Tamriel.
- **Moving platforms and doors** (keyframed bodies) don't move; load doors don't lead anywhere yet.
- **LE packed-strips collision** (`bhkPackedNiTriStripsShape`) is decoded per nif.xml but untested: vanilla SE data has none.
- Ragdolls, constraints, phantoms (triggers) and character-vs-character collision are not imported or simulated.
