# .okc — kill camera models (kind `KCAM`, version 1)

An SPUD container (`container.md`), written by the importer to `rules/cameras.okc`: every `Cameras\*.nif` a CAMS record names, so the engine needs no NIF. Reader and writer: `src/formats/okc.hpp`. What the data means: ` and 5.

| Chunk | Contents |
|---|---|
| `MODS` | `{path u32, frustum right f32, rotates u32, first frame u32, frame count u32}` per model. Path is a string offset, lower case, relative to `meshes`, forward slashes |
| `FRMS` | `{translation 3 x f32, angles 3 x f32, right f32}` per frame: the root's local translation (Skyrim units), Euler angles (radians, x y z) and the frustum's right as the game drives it (0 for none), every 1/30 s from time 0 to the last key |
| `STRS` | String table |

The source's keys are TCB, quadratic or linear; they are sampled, so the engine reads two frames and interpolates. Two models (`base.nif` and one other) move along a `NiPathInterpolator`: position keys walked by a percent track, taken evenly in key time (*inferred*). `rotates` is whether the root has rotation keys. The frustum is 4:3: the vertical half tangent is `frustum right` x 0.75.
