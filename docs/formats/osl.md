# .osl — loading screens (kind `LSCR`, version 1)

Layout: [`src/formats/osl.hpp`](../../src/formats/osl.hpp) (records with `static_assert`ed sizes are the source of
truth). Written by the importer (`skytool ui import`, and `skytool import`) to `<cache>/interface/loadingscreens.osl`
from the load order's LSCR records and the records their conditions read; read by the Loading Menu
([`ui/loading_screens.hpp`](../../src/ui/loading_screens.hpp)). How the game uses each field:
.

| Chunk | Contents |
|---|---|
| `SCRN` | `LoadScreen[]` — every LSCR, in the order the game's form array holds them |
| `COND` | `Condition[]` — the screens' CTDA items, each screen's a contiguous run |
| `LOCN` | `Location[]` — every LCTN: form ID, EDID, parent (PNAM) |
| `WRLD` | `Worldspace[]` — every WRLD: form ID, EDID, its own location |
| `WCEL` | `WorldCell[]` — the worldspaces' cell → location maps |
| `ICEL` | `InteriorCell[]` — interior cells and the location they lie in |
| `QUST` | `Quest[]` — the quests conditions name: DNAM flags, start-up stage, stages |
| `QSTG` | `QuestStage[]` — their stages (INDX index and flags) |
| `GLOB` | `Global[]` — the globals conditions name, with their starting value |
| `QVAR` | `QuestVariable[]` — Papyrus quest variables conditions name, with their starting value |
| `PKWD` | `u32[]` — the player's starting keywords (sorted) |
| `DEAD` | `u32[]` — references conditions name that start dead (sorted) |
| `FLST` | `FormListEntry[]` — members of the form lists conditions name |
| `STRS` | string table |

All form IDs are resolved into the load order (the plugin index the game gives them). Strings are offsets into
`STRS`; `0xFFFFFFFF` means none.

## Records

### LoadScreen (80 bytes)

`form_id`, `edid`, `text` (DESC through the plugins' string tables, UTF-8), `model` (the NNAM static's MODL as a
cache path, `.osm`), `camera_path` (MOD2 as a cache path, `.ose`), `record_flags` (0x400 "displays in main menu"),
`scale` (SNAM, 1 when absent), `rotation` (RNAM, three int16 degrees), `rotation_min`/`rotation_max` (ONAM, int16
degrees, −180/180 when absent), `offset` (XNAM, Skyrim units), `first_condition`/`condition_count`, `static_form`
(NNAM's form ID; the game counts a screen as having a model when this is set), `has_inv_marker` and `inv_rotation`
(the model's root carries the BSInvMarker named `INV`: its three uint16 times 0.001, radians).

Order: the plugins in load order, each in file order; a record overridden by a later plugin keeps the position of its
first version and takes the data of the winning one. Deleted records are left out.

### Condition (32 bytes)

The CTDA as the game holds it: `function` (index = script opcode − 0x1000), `flags` (byte 0: compare operator in bits
5–7, 0x10 swap subject and target, 0x08 package data, 0x04 the value is a global, 0x02 use aliases, 0x01 OR), `run_on`
(0 subject … 7 event data), `value` (the comparison value; with flag 0x04 `value_global` is the global's form ID),
`param1`/`param2` (form IDs for the parameters the function takes as forms, otherwise as stored), `reference` (run-on
2), `param3`, `string_param` (CIS1/CIS2 that follows the CTDA).

### Location, Worldspace, WorldCell, InteriorCell (16 bytes each)

- `Location`: form ID, EDID, parent location (0 for none).
- `Worldspace`: form ID, EDID, `location` = XLCN, else the location of its encounter zone (XEZN → ECZN DATA), else 0.
- `WorldCell`: worldspace form ID, cell x and y, location form ID. Built as the game's loading builds the map:
  every version of every LCTN in load order; LCEC and ACEC (a worldspace, then 32-bit keys `x << 16 | (y & 0xFFFF)`)
  insert or replace, RCEC removes.
- `InteriorCell`: form ID, EDID, location = its encounter zone's location, else XLCN, else 0.

### Quest (32 bytes), QuestStage (4 bytes)

Quest: form ID, EDID, DNAM flags (0x0001 start game enabled), `startup_stage` (the first INDX flagged 0x02, −1 for
none), its stages. QuestStage: INDX stage index, flags.

### Global (16), QuestVariable (16), FormListEntry (8)

- `Global`: form ID, EDID, FLTV.
- `QuestVariable`: quest form ID, the variable's name as the condition gives it (`::Name_var`), its value as a new game
  starts it, and where it came from: 1 the quest's VMAD property `Name`, 2 the compiled script's initial value of the
  variable (`scripts/<script>.pex`), 0 not found (value 0).
- `FormListEntry`: list form ID, member form ID (FLST LNAM, in order).

## Tools

`skytool ui loading [filter]` prints the screens (EDID filter), their conditions, quests and quest variables.
