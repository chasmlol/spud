# .osu — UI movies, string tables, font maps and controls (version 2)

Layout: [`src/formats/osu.hpp`](../../src/formats/osu.hpp) (struct sizes are `static_assert`ed there and are
the source of truth). All four kinds are [SPUD containers](container.md) and share one version number. The
importer writes them to `<cache>/interface/`, mirroring the game's `interface/` paths (`interface/startmenu.swf`
→ `interface/startmenu.osu`). The player is `src/ui/player`.

| Version | Change |
|---|---|
| 1 | First version. |
| 2 | `FACT` entries are `FrameAction` (function, place commands before it) instead of bare function indices. |

`skytool ui import` converts a movie again when its `.osu` is missing or of another version.

Next to the `.osu` files the importer also writes, for the menus:

| File | Contents |
|---|---|
| `interface/gamesettings.osu` | A `UITX` table of the load order's game settings (GMST, winning versions). Strings have their localized text resolved through the plugins' `.STRINGS` tables, cp1252 converted to UTF-8. Numbers (`i`, `u`, `f`, `b` settings) are stored as decimal text. |
| `interface/globals.osu` | A `UITX` table of the load order's global variables (GLOB, winning versions): name and starting value as decimal text. |
| `interface/soundcategories.osu` | A `UITX` table of the sound categories the Journal's Audio page lists (SNCT with FNAM bit 1, winning versions, in the data handler's order): for the n-th, `NNN.id` (form ID, 8 hex digits), `NNN.name` (FULL, localized) and `NNN.default` (UNAM, the default menu value, 0..65535), NNN three decimal digits from 000. |
| `interface/controls.osu` | Kind `UICT`: the control map and key names (below). |
| `interface/credits.txt`, `interface/credits_plru.txt` | Copied as they are: lines of HTML the Credits Menu appends. |

A movie is a compiled Flash movie: shapes and glyphs are already triangles, bitmaps are decoded, and the
script is translated into resolved instructions. Coordinates are stage pixels (the source's twips / 20), y down.

## Kind `UIMV`: a movie

| Chunk | Contents |
|---|---|
| `HEAD` | `Header`: stage size, frame rate, root sprite (always 0), background colour, source SWF version |
| `STRS` | String table (NUL-terminated UTF-8, addressed by offset) |
| `CHAR` | `Character[]`: source id, kind (shape, sprite, text, button, bitmap, font, imported) and index into that kind's table |
| `SHAP` `MESH` `VERT` `INDX` | Shapes: bounds and a run of meshes. A mesh is one fill or stroke: triangles (`INDX`, relative to its first vertex), fill kind and colour / gradient / bitmap, and `fill_matrix` from shape space to fill space. Glyphs share `VERT` and `INDX`. |
| `GRAD` `GSTP` | Gradients: spread (pad, reflect, repeat), interpolation, focal point, stops (ratio 0–1, RGBA8) |
| `BMAP` `BDAT` | Bitmaps: size and offset into `BDAT`; RGBA8, premultiplied, rows top-down |
| `SPRT` `FRAM` `LABL` | Timelines. Sprite 0 is the main timeline. A frame lists its place commands, frame-action blocks (`FACT`) and init-action blocks (`INIT`). Labels are (sprite, frame, name). |
| `FACT` | `FrameAction[]`: a DoAction block's function and `after`, how many of its frame's place commands came before its tag. The player queues each block between the load events of the objects placed before and after it (tag order, as Flash Player processes a frame). |
| `PLCE` | `PlaceCommand[]`: place, move or remove at a depth, with matrix, colour transform, ratio, instance name, clip depth (mask), filters, blend mode, cache-as-bitmap, visibility and clip events, each present when its `PlaceFlag` is set |
| `FILT` `FVAL` | Filters (drop shadow, blur, glow, bevel, gradient glow/bevel, convolution, colour matrix) and their extra values |
| `EVNT` | Clip events: source event flags, key code, handler function |
| `RECT` | Scale-9 grids, referenced by sprites |
| `TEXT` | Edit texts: bounds, flags, font (character or imported name), size, colour, alignment, margins, indent, leading, variable name, initial text |
| `FONT` `GLYF` `KERN` | Fonts: name, bold/italic, metrics in em, glyphs sorted by code with advance, bounds and triangles in em units (origin on the baseline, y down), kerning pairs |
| `BUTN` `BREC` `BACT` | Buttons: state records and condition actions |
| `EXPT` `IMPT` | Exported names (including `__Packages.*` class sprites) and imported characters (source movie and name) |
| `FUNC` `PARM` `CODE` `PVAL` | Script: functions, their parameters, instructions and push values |

### Script

Every action block (frame, init, clip event, button condition) and every function body is a `Function`. Kinds:
`block`, `function` (DefineFunction) and `function2` (DefineFunction2, with its register count and
preload/suppress flags). Parameters carry the register a `function2` argument goes to (0: by name only).

Instructions keep ActionScript 2 opcode numbers and stack semantics (Adobe's SWF 10 specification, "Actions").
What the source encodes positionally is resolved at import:

| Op | `count` | `arg` |
|---|---|---|
| `Push` (0x96) | number of values | first value in `PVAL` (string, number, null, undefined, register, boolean) |
| `Jump` (0x99), `If` (0x9D) | | target instruction index within the function (`instruction_count` = end) |
| `DefineFunction` (0x9B), `DefineFunction2` (0x8E) | | the child function |
| `StoreRegister` (0x87) | | register |
| `GotoFrame` (0x81) | | frame |
| `GoToLabel` (0x8C), `SetTarget` (0x8B) | | string |
| `GotoFrame2` (0x9F) | play flag | scene bias |
| `GetURL2` (0x9A) | method flags | |

`ConstantPool` does not appear: its strings are folded into the push values, using the pool in effect where each
function is defined, which is what the function sees when it runs. `GetURL` becomes a `Push` of URL and target
followed by `GetURL2`. All other ops take their operands from the stack.

## Kind `UITX`: translations

`STRS` plus `TRAN`: `Translation[]` (key, text), sorted by key. Keys include the `$`. From
`interface/translate_<language>.txt` (UTF-16, `$KEY<tab>text` lines).

## Kind `UICT`: controls

From `interface/controls/pc/`: `keyboard_<language>.txt`, `mouse.txt` and `gamepad.txt` (key name, hardware
value, Flash key code) and `controlmap.txt` (user events per input context). `STRS` plus:

| Chunk | Contents |
|---|---|
| `CKEY` | `InputKey[]`: key name, device (keyboard, mouse, gamepad), the Flash key code the menus see (or none) and the device code (DirectInput scan code for keys, the control map's ids for buttons) |
| `CCTX` | `ControlContext[]`: the input contexts in file order ("Main Gameplay" is 0, "Menu Mode" 1, ...), each a run of bindings |
| `CBND` | `ControlBinding[]`: user event, device, remappable, device code (a chord keeps its last key) and the event's group flags |

## Kind `UIFM`: font map

`STRS`, `FLIB`: library movies in load order (cache paths, e.g. `interface/fonts_en.osu`), and `FNAM`: logical
name (`$EverywhereFont`), font name in a library (`Futura CondensedLight`) and style flags. From
`interface/fontconfig*.txt`.
