# .osb — behaviour data (kind `BEHV`, version 1)

An SPUD container (`container.md`) holding one project, character or behaviour graph as an object graph, written by the
importer to `animations/meshes/...`. Clip motion by clip generator name uses the same file, at
`animations/motion/<project>.osb`. Reader and writer: `src/formats/osb.hpp`.

Objects are named by class and their members by name, as ` lists them. Nothing about the
source's memory layout survives: pointers are object indices and inline structs are objects too. Objects of classes
the importer's tables leave out (physics) are dropped, and members pointing at them read as none. The animation runtime
builds its nodes from these objects when a project loads (`src/anim/graph.cpp`).

| Chunk | Contents |
|---|---|
| `INFO` | `u32` the root object: the `hkRootLevelContainer`, or `~0` in a motion file |
| `OBJS` | `ObjectRecord[]`: class (string offset), first value, value count |
| `VALS` | `ValueRecord[]`: name (string offset), kind, `a`, `b` |
| `INTS` | `i32[]`: arrays of integers |
| `FLTS` | `f32[]`: arrays of reals, and vectors, quaternions and transforms as their floats |
| `BYTS` | `u8[]`: arrays of bytes |
| `STRS` | String table |

A value's `a` and `b` by kind:

| Kind | Meaning | `a`, `b` |
|---|---|---|
| 0 none | a null pointer or no value | |
| 1 integer | integer, bool, enum, flags | low and high words |
| 2 real | | the float's bits |
| 3 text | | string offset |
| 4 object | | object index |
| 5 list | arrays of objects, strings and structs | first value in `VALS`, count |
| 6 ints | | first element in `INTS`, count |
| 7 floats | | first element in `FLTS`, count |
| 8 bytes | | first element in `BYTS`, count |

Floats of a `qstransform` are translation x y z w, rotation x y z w, scale x y z w. In a motion file each object is a
`clip_motion` with `name`, `translation` (time x y z per key) and `rotation` (time x y z w per key); times are seconds,
rounded to half floats as the game holds them.

A skeleton with a ragdoll has `<skeleton>.ragdoll.osb` beside its `.osk`: the file's whole graph, physics classes included,
with the skeleton NIF's bodies and joints written over it. Lengths are game units.

## Versions

- 1: first version.
