# Forms database (`.osf`)

Every form of a load order after overrides, in one file: `<cache>/forms.osf`. Written by the importer
(`src/importer/records`), read by everything else through `src/formats/forms.hpp`. Version 3. Version 2 had no
`SGRP`; version 1 had no `layers` either (the byte was reserved, and a deleted winner held only its own
fields).

An SPUD container (`container.md`) of kind `FORM`. Each form is stored once: the version of the last plugin that
defines it (deleted ones included, flagged), and when that version is deleted, the versions the game loads it on
top of (`layers`, below). Its fields keep the plugin's order and multiplicity, in a canonical
encoding described by the record schema, which the file carries.

## Chunks

| Chunk | Contents |
|---|---|
| `SCHM` | The record schema, UTF-8 text in the language below. Every field of every form is laid out as it says. |
| `COND` | The condition function table: `param <type id> <kind> <name>` and `function <index> <name> <param type ids...>` lines, as extracted from the game's script function table. Tells what each `CTDA` parameter holds. |
| `HEAD` | `Header`: plugin and form counts, data size, the language the text was resolved with. |
| `PLUG` | `PluginEntry[]`, load order. Name, flags (master, light, localized), mod index / light index, masters, record count, author and description, XXH64 of the file, and the plugin's own `TES4` fields. |
| `MAST` | `u16[]`: plugin indices of each plugin's masters (`PluginEntry::first_master`, `master_count`). |
| `STRS` | String table for names in `PLUG` and `HEAD` (offsets, `container.md`). |
| `FRMS` | `FormEntry[]` sorted by FormID. |
| `SRCS` | `u16[]`: for each form, every plugin that defines it, in load order (`first_source`, `source_count`); the last is the winner. |
| `SGRP` | `u8[]`, parallel to `SRCS`: the type of the group each of those records sat in (8 persistent and 9 temporary cell children,...). Decides which versions the game reads. |
| `DATA` | Field streams (below). |
| `FIXS` | `u32[]`: for each form, the offsets (from its `data_offset`) of every FormID in its fields. Lets tools find every reference without decoding. |

Struct layouts (`Header`, `PluginEntry`, `FormEntry`) are in `src/formats/forms.hpp` with their sizes asserted.

### Forms

`FormEntry` holds the FormID (load-order resolved), record type, the winning record's header flags and form
version, the winning plugin, the owner of the group the record sat in (`parent`: the `WRLD` of a `CELL`, the
`CELL` of a reference, `LAND` or `NAVM`, the `DIAL` of an `INFO`) and that group's type (`group`: 8 persistent
and 9 temporary cell children, ...), the record's index in its plugin (`file_order`, which keeps the plugins'
ordering of `INFO`s within a topic), the version-control stamp, and where its fields and FormID positions are.

`layers` is how many records' fields the form's data holds, in load order, one after the other: 1 for almost
every form. The game loads a deleted record on top of the form's earlier data instead of replacing it
, so for a deleted winner the data holds every version the
game reads from the last non-deleted one (or the first, if all are deleted) through the winner, and `layers`
counts them (at most 255). A master's temporary cell child is never layered: the game reads only its winner. A reader that wants a field's value takes its last occurrence (`forms::Form::get`); a list the game
appends to (a spell's effects) is every occurrence (`forms::Form::all`). FormID fixups cover all layers.

FormIDs everywhere are load-order resolved, as the game shows them: `0xMMxxxxxx` for full plugins (`MM` = mod
index), `0xFEnnnxxx` for light plugins (`nnn` = light index). 0 is "none".

Record flags are stored as the plugin had them; `0x20` (deleted) is the one readers must check. Deleted forms
stay in `FRMS` with their (usually empty) fields so that overrides and their sources are complete.

### Fields

A form's data is a sequence of fields, each `u32 signature`, `u32 size`, then `size` bytes, in the order the
plugin stored the subrecords. A field's bytes are the subrecord's, except:

- **FormIDs** are load-order resolved.
- **Text** (`zstring`, `lstring`, `wstring`, `wstring32` in the schema) is UTF-8. Localized strings hold their
  resolved text, NUL-terminated, never a string ID: a reader never needs string tables.
- Compression and `XXXX` size prefixes are gone (sizes are `u32`).
- Bytes after the terminator of a field's trailing string (left by authoring tools, never read by the game) are
  dropped.

A field that the schema doesn't describe, or whose bytes don't fit the schema, is stored exactly as in the plugin
(FormIDs unresolved). `skytool records check` reports every such field; for the shipped schema and the user's load
order the target is none.

`forms::decode_form()` (`src/formats/forms_value.hpp`) decodes any form's fields into a value tree by the schema;
`skytool records show <id>` prints it as JSON.

## Schema

The schema is plain text, shared by the importer (which converts plugin records with it) and readers (which
decode fields with it). The importer's copy is `src/importer/schema/*.txt`, compiled in (`common.txt` first, then
the rest in name order; a field set must be defined before it is used); every database carries the text it was
written with.

```
# comment to end of line
type NAME = TYPE            # a named type; NAME starts with an upper-case letter and isn't four capitals
fields NAME                 # a reusable group of field definitions
  FIELD...
end
record SIG "description"
  FIELD...
  use NAME                  # insert a field group
end

FIELD := SIG name TYPE [when $var=V[|V...]] ... [-> $var[=.member|=V]]
```

A record lists every subrecord its plugins can contain. Order in the schema doesn't matter for decoding: each
subrecord in a plugin is matched to the first definition with its signature whose `when` conditions hold. A
signature may be listed several times with different conditions when its layout depends on context.

### Types

| Type | Plugin bytes | Canonical bytes |
|---|---|---|
| `i8 i16 i32 i64 u8 u16 u32 u64 f32 f64` | little-endian | same |
| `formid` / `formid(SIG,SIG,...)` | `u32` FormID relative to the plugin's masters; the list names the record types it may point to | resolved `u32` |
| `zstring` | NUL-terminated text (a field's last string may end without NUL) | UTF-8, NUL-terminated |
| `zstring(N)` | a fixed N-byte buffer holding NUL-terminated text (bytes after the NUL are stale) | UTF-8 text, zero-padded to N bytes |
| `lstring` `lstring.dl` `lstring.il` | localized: `u32` string ID into the plugin's `.STRINGS` / `.DLSTRINGS` / `.ILSTRINGS` when the plugin is localized, else a `zstring` | UTF-8, NUL-terminated |
| `wstring` / `wstring32` | `u16` / `u32` byte length, then text | same length type, UTF-8 |
| `bytes(N)` / `bytes` | N bytes / the rest of the field | same |
| `empty` | nothing (marker subrecords) | nothing |
| `{ name:TYPE name:TYPE ... }` | members in order | same |
| `TYPE[]` | elements to the end of the field | same |
| `TYPE[N]` | N elements | same |
| `TYPE[u8]` `TYPE[u16]` `TYPE[u32]` | count prefix, then elements | same |
| `TYPE[.member]` | count taken from an earlier member of the enclosing structs | same |
| `union(SELECTOR) { V: TYPE  V|V: TYPE  *: TYPE }` | one of the types, chosen by the selector's value | same |
| `codec(NAME)` | a layout decoded by code (below) | below |
| `NAME` | a named type | |

Union selectors: `size` (the field's size in the plugin), `formver` (the record's form version), `.member` (an
earlier member of the enclosing structs), `$var` (a record context variable). Case values are integers, or quoted
strings, which match a variable set from a string field (`string_key()`: FNV-1a of the lowercased text).

Context variables: `-> $var` after a field sets `$var` once that field is decoded: to its first scalar member (or
the string key of its first string), to a named member with `=.member`, to the character code of its first
string's first character with `=char0` (GMST: the EditorID's first letter gives the setting's type), or to a
constant with `=V`. Variables
start unset for every record; `when $var=V` on a field definition, and `union($var)`, read them.

A structure at the top of a field may end early: plugins written by older tools store shorter versions of many
structures, and the game reads what is there. Members after the end are absent (readers treat them as zero).

`_` names padding and bytes the game never uses; readers skip them.

### Codecs

| Codec | Field | Canonical form |
|---|---|---|
| `ctda` | Condition item (`CTDA`) | Same layout as the plugin (up to 32 bytes: operator and flags, comparison value or `GLOB`, function index, parameters 1 and 2, run-on, reference, parameter 3). Which parameters are FormIDs depends on the function (`COND` chunk) and the flags; those, the global and the reference (run-on 2) are resolved. |
| `vmad` | Script attachments (`VMAD`) | Same layout with every string as `wstring` UTF-8: version, object format, scripts (name, flags, properties of type object / string / int / float / bool / arrays of those), then for `QUST`, `INFO`, `PACK`, `PERK` and `SCEN` the script fragments (and quest alias scripts). Object properties' FormIDs are resolved. |
| `coed` | Container item extra data (`COED`) | Same 12 bytes: owner, then a `GLOB` (resolved) if the owner is an `NPC_` or a required rank if it is a faction, then item condition. |

## Reading

`forms::Database` (`src/formats/forms.hpp`) memory-maps a database: `find(form_id)`, `of_type(sig)`,
`fields(entry)`, `field(entry, sig)`, `editor_id(entry)`, `sources(entry)`, `source_groups(entry)`, `plugins()`,
`fixups(entry)`.

Every field of every record is reachable by its schema name: `forms::Form` (`src/formats/forms_typed.hpp`) decodes
a form's fields once (through `forms::decode_form`, `forms_value.hpp`) and answers `get("DATA.value")`,
`get("game_data.weight")`, `all("KWDA")`, `name()`, `description()`, `keywords()`. The names are the ones in
`src/importer/schema/*.txt` (and in the database's `SCHM` chunk).

Typed accessors for what the gameplay systems read most:

| Accessor | Records |
|---|---|
| `item_value`, `item_weight` | ALCH, AMMO, APPA, ARMO, BOOK, INGR, KEYM, LIGH, MISC, SCRL, SLGM, WEAP |
| `container_items` | CONT, NPC_ |
| `leveled_list` | LVLI, LVLN, LVSP |
| `effects` | SPEL, ENCH, ALCH, INGR, SCRL |
| `form_list` | FLST |
| `global_value`, `setting_value` | GLOB, GMST |
| `placement` | REFR, ACHR, placed projectiles and hazards |
| `responses`, `shared_info` | INFO |
| `quest_stages` | QUST |
| `faction_ranks`, `npc_factions` | FACT, NPC_ |
| `navmesh` | NAVM: vertices, triangles (vertices, edges, flags, cover flags), edge and door links, cover triangles, search grid |

The values are what the plugins say, after overrides. Rules the game applies on top (templates, enable parents,
leveled actors,...) belong to the systems that use them; ` lists the ones verified so far.
