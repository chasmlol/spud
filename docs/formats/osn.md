# .osn — animation clip (kind `CLIP`, versions 1 and 2)

An SPUD container (`container.md`): one clip, written by the importer to `animations/meshes/...`. Tracks stay in the
compressed B-spline blocks of the source, so sampling gives the source's values at any time; the layout of a block is in
`, "Spline compression", and `src/anim/clip.cpp` reads it. Reader and writer:
`src/formats/osn.hpp`.

| Chunk | Contents |
|---|---|
| `INFO` | duration (s), transform tracks, float tracks, frames, blocks, max frames per block, mask size, block duration, block inverse duration, frame duration, additive (0 or 1) |
| `BLKS` | `u32[]`: each block's offset into `DATA` |
| `FLBK` | `u32[]`: each block's float track data, from the block's start |
| `DATA` | The blocks |
| `TRBN` | `i16[]`: the bone each transform track drives; empty: track i drives bone i |
| `FLSL` | `i16[]`: the same for float tracks |
| `ANNO` | Annotations by time: time (s), text (string offset) |
| `TRNM` | `u32[]`: the annotation tracks' names (string offsets), one per transform track |
| `RAWT` | Version 2 only: `f32[]`, uncompressed: frames x transform tracks x 12 (translation, rotation, scale, as hkQsTransform), with no blocks |
| `STRS` | String table |

Lengths are game units, as the source's.

## Versions

- 1: first version.
- 2: `RAWT`, for clips with no spline blocks. Only such clips are written as 2.
