# .ost — saved game (kind `SAVE`, version 3)

Layout: [`src/formats/ost.hpp`](../../src/formats/ost.hpp). One file per save, in `Documents\My Games\spud\Saves`, named as says. A save holds no content: the plugins it was made on name the content, and each section is one system's change layer over it, what differs from the records, keyed by FormID. A reader skips sections it doesn't know.

| Chunk | Contents |
|---|---|
| `HEAD` | `Header`: kind (manual, autosave, quicksave), next manual number, character id, level, days passed, play seconds, time saved, load-order hash, picture size, modded |
| `TEXT` | player name, location text, race, the space's name: length-prefixed strings |
| `PLUG` | the load order: count, then name and content hash per plugin |
| `THMB` | the picture, RGBA8 |
| `SECT` | `u32` FourCCs: the sections, in order |
| `CLOK`, `WSIM`, `PLYR`, `GAME`, `ACTR`, `SKY_`, `PVM_`, `QUST`, `UI__`, `SEEN`, `DEST`, `NGER` | the sections. Sets and maps are written in FormID order, so the same state is the same bytes |

Strings in sections are `u32` length then bytes. A section's layout is its system's `save` and `load`.

Version 2: the `k_game` section ends with what scripts changed of the world: the references' own enable state (u32 count, then form and a byte), misc stats (count, then string and i32), moved and placed references (count, then form, base or 0, position), and what scripts added to leveled lists (count, then list, form, u16 level, u16 count).
