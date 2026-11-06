# .ofm — face morphs (kind `FMOR`, version 1)

Layout: [`src/formats/ofm.hpp`](../../src/formats/ofm.hpp). The morphs of one head part, beside the skinned models in the cache (`skinned/meshes/actors/character/character assets/malehead.ofm`), converted from a FaceGen `FRTRI003` file.

| Chunk | Contents |
|---|---|
| `HEAD` | vertex count (u32), morph count (u32), bytes of the uncompressed deltas (u32), reserved (u32) |
| `NAME` | the morphs' names, NUL-terminated, in file order |
| `SCAL` | `f32[morphs]`: metres per delta unit (the file's scale × the game unit) |
| `DATA` | the deltas, LZ4-compressed: morph-major `i16[morphs][vertices][4]`, x y z and a zero, 8 bytes a vertex so the GPU reads them whole |

## Semantics

- A vertex moves by `delta × scale × weight`; morphs add. Only positions change.
- Vertices are in the head part shape's order, which a skinned head model keeps from its NIF (docs/formats/osm.md).
- Names keep their case and their first spelling: where a file repeats a name, the first one wins, as the game's lookup does.
- Stat morphs (the file's "modifiers") aren't kept: no vanilla head `.tri` has any.

## Versions

- 1: first version.
