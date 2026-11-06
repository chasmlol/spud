# .ose — particle effect and scene graph (kind `PSYS`, version 6)

Layout: [`src/formats/ose.hpp`](../../src/formats/ose.hpp) (records with `static_assert`ed sizes are the source of
truth). Written by the importer next to the mesh converted from the same NIF (`meshes/x/y.nif` →
`meshes/x/y.osm` + `meshes/x/y.ose`, lower case) for every NIF that holds a decodable `NiParticleSystem`; read by
the simulation in `src/fx` ([`fx/effect.hpp`](../../src/fx/effect.hpp)). Behaviour.

Version 2 adds node animation: transform interpolators and the targets of NiMultiTargetTransformController. The
importer also writes an `.ose` for a NIF whose content is animated nodes without particle systems (the loading
screens' camera paths, `meshes/cameras/*.nif`). Readers accept version 1
files, which have none of the version 2 chunks.

Version 3 adds what the UI 3D scenes read: each node's class and a shape's bound and UV
rectangle (`NODX`), the node every .osm submesh was converted from (`SUBN`) and the root's "INV" marker (`INVM`).
`skytool ui import` and `skytool ui model` write an `.ose` for every UI model, with or without particles. Readers
accept versions 1 to 4; a UI scene needs a version 3 or later file (one with `NODX`).

Version 4 has version 3's layout under a new name. The format was `.osp` until the UI workstream was merged with the
movement workstream, whose physics shapes (kind `PHYS`) keep `.osp` and are written next to the same meshes, so
the cache needs its particle effects re-imported once.

Version 5 adds `ATCH`: the root's `AttachT` strings, which say where art goes on its target (
12). Readers accept versions 1 to 5.

| Chunk | Contents |
|---|---|
| `INFO` | root node and the record counts (cross-checked on read) |
| `ATCH` | `u32[]` — string offsets, the root's `AttachT` techniques, `Name` or `Name&Arg` (version 5) |
| `ADDN` | `AddonNode[]` — only in files with any: the file's `AddOnNode<index>` nodes, each with its index, the longest stop − start of the file's controllers (below 0 without any), rotation, translation and scale in the model's space. A file may hold these and no systems: most impact models are nothing else |
| `NODE` | `Node[]` — every NiAVObject of the NIF: name, parent, flags, local transform |
| `SYST` | `System[]` — one per NiParticleSystem: node, material, max particles, array flags, modifier range, sub-texture range, aspect fields, stored bound |
| `MODS` | `Modifier[]` — every converted modifier and emitter, grouped by system in update order |
| `FLTS` | `f32[]` — scale-modifier curves |
| `SUBT` | `f32x4[]` — sub-texture records (the NiPSysData "subtexture offsets") |
| `EMSH` | `EmitterMesh[]` — shapes mesh emitters sample |
| `EMPO`, `EMNO` | `f32x3[]` — emitter-mesh positions and normals |
| `EMTR` | `u16x3[]` — emitter-mesh triangles (indices relative to the mesh's first vertex) |
| `CTLR` | `Controller[]` — every controller, in the order the game's scene update reaches them |
| `INTP` | `Interpolator[]` |
| `KEYG` | `KeyGroup[]` — a float or bool key array of an interpolator |
| `FKEY`, `BKEY` | `FloatKey[]`, `BoolKey[]` |
| `SEQS` | `Sequence[]` — the NiControllerManager's controller sequences |
| `SBLK` | `Block[]` — a sequence's controlled blocks |
| `TKEY` | `TextKey[]` — a sequence's text keys ("start", "end", ...) |
| `MATL` | `EffectMaterial[]` — BSEffectShaderProperty + NiAlphaProperty per system, and per animated mesh shape |
| `TINT` | `TransformInterp[]` — version 2: NiTransformInterpolator poses and their keys |
| `VKEY` | `Vec3Key[]` — version 2: translation keys |
| `QKEY` | `QuatKey[]` — version 2: quaternion rotation keys |
| `CTGT` | `u32[]` — version 2: NiMultiTargetTransformController targets (node indices) |
| `NODX` | `NodeExtra[]` — version 3, parallel to `NODE`: kind (other, node, billboard, shape), billboard mode, a shape's bound sphere in its own space (the stored one; a skinned shape's from its bones at the file's rest pose), a shape's UV rectangle (u min, u max, v min, v max, clamped to [0, 1]; (0, 1, 0, 1) without UVs) |
| `SUBN` | `u32[]` — version 3: per .osm submesh, the shape node it was converted from |
| `INVM` | `InvMarker[]` — version 3, 0 or 1: the root's BSInvMarker "INV" (rotation x, y, z in radians x 1000, zoom) |
| `STRS` | string table |

Every index a record holds is range-checked by `ose::read` and `ose::write`; `k_none` (`0xFFFFFFFF`) means none.

## Units and state

- Values keep the **game's own units**: lengths in Skyrim units (1 unit = 0.0142875 m), times in seconds, angles in
  radians, so the simulation can follow the game's float arithmetic. The simulation API converts its output
  to metres.
- Values are the state the game holds **after loading** (its `LoadBinary`), not the raw stream where the two differ:
  - controller `flags`: `(file & 0xFF3F) | 0x40` (the game clears force-update, sets
    compute-scaled-time);
  - `lod` modifier: begin/end distances multiplied by 0.33333334 and the order forced to 1;
  - modifiers sorted by order, ties in file order.
- Node rotations are 3x3 column-major: `parent = rotation * (scale * local) + translation`.

## Records

### System flags

`world_space` 0x1, `colors` 0x2, `radii` 0x4, `sizes` 0x8, `rotations` 0x10, `rotation_angles` 0x20,
`rotation_axes` 0x40, `texture_indices` 0x80, `rotation_speeds` 0x100 — the NiPSysData "has" flags, i.e. which
per-particle arrays exist (radii and sizes always exist at runtime).

### Modifier

Common: `type`, `name`, `order`, `system`, `active`. Per type (`f` = the float array, in this order):

| Type | Fields |
|---|---|
| `age_death` (1) | `flag0` spawn on death; `ref` the spawn modifier (index into modifiers) |
| `lod` (2) | `f` begin, end (load-time scaled), end emit scale, end size |
| `volume_emitter` (13) | `f[0..13]` as `mesh_emitter`; `f[14..16]` extents (sphere: radius; box: width, height, depth; cylinder: radius, height); `u0` shape (0 sphere, 1 box, 2 cylinder); `ref` the emitter object |
| `mesh_emitter` (3) | `f[0..5]` speed, speed variation, declination, declination variation, planar angle, planar angle variation; `f[6..9]` initial colour; `f[10..11]` radius, radius variation; `f[12..13]` life span, life span variation; `f[14..16]` emission axis; `u0` initial velocity type (0 normals, 1 random, 2 direction); `u1` emission type (0 vertices, 1 face centre, 2 edge centre, 3 face surface, 4 edge surface); `first`/`count` its emitter meshes |
| `spawn` (4) | `u0` generations; `u1` min | max << 16; `f` percentage, speed variation, direction variation, life span, life span variation |
| `simple_color` (5) | `f[0..5]` fade in, fade out, colour 0→1 start, end, colour 1→2 start, end; `f[6..17]` three RGBA colours |
| `rotation` (6) | `f[0..3]` speed, speed variation, angle, angle variation; `f[4..6]` axis; `flag0` random speed sign; `flag1` random axis |
| `subtex` (7) | `f[0..6]` start frame, start frame fudge, end frame, loop start frame, loop start fudge, frame count, frame count fudge |
| `scale` (8) | `first`/`count` the curve in `FLTS` |
| `gravity` (9) | `ref` the gravity object (node); `f[0..2]` axis; `f[3]` decay; `f[4]` strength; `f[5]` turbulence; `f[6]` turbulence scale; `u0` force type (0 planar, 1 spherical); `flag0` world aligned |
| `drag` (10) | `ref` the drag object (node); `f[0..2]` axis; `f[3]` percentage; `f[4]` range; `f[5]` range falloff |
| `position` (11) | — |
| `bound_update` (12) | `u0` update skip |

### Controller

`type`: `modifier_active` 1, `emitter` 2, `update` 3, `effect_float` 4, `effect_color` 5, `transform` 6
(NiMultiTargetTransformController), `other` 7 (any other controller, stored with its type name in `name`). A
`transform` controller's targets are `controller_targets[first_target, + target_count)` in file order (version 1
kept these words reserved); the simulation gives each target a manager-controlled blend interpolator, which the
sequence blocks naming that node drive.
Timing fields are NiTimeController's (`start_time`/`stop_time` are the key range). `target_kind`/`target`: a system,
a material or a node. `interpolator` is the controller's interpolator (the emitter's birth rate);
`interpolator2` the emitter's visibility ("EmitterActive") interpolator. `modifier` resolves the modifier name of
NiPSysModifierCtlr types; `variable` is BSEffectShaderPropertyFloatController's controlled variable (0 base colour
scale, 1/2 falloff start/stop angle, 3/4 falloff start/stop opacity, 5 alpha, 6/8 U/V offset, 7/9 U/V scale).
Types 5–7 are stored but not simulated.

### Interpolator and keys

`type`: float 1, bool 2, bool timeline 3, blend float 4, blend bool 5, transform 6 (`key_group` indexes `TINT`). `float_value` (−FLT_MAX: none) and
`bool_value` (2: none) are the streamed values; blend interpolators keep `blend_flags` (bit 0 manager controlled),
`array_size` and `weight_threshold`. `key_group` points at a `KeyGroup` (`kind` float 1 / bool 2, `key_type` 1
linear, 2 quadratic, 3 TBC, 5 constant). Float keys hold time, value, the quadratic tangents (`forward` = in,
`backward` = out) and TBC parameters.

### TransformInterp, Vec3Key, QuatKey

`TransformInterp` (64 bytes): the interpolator's own pose (translation, rotation as quaternion w x y z, scale;
−FLT_MAX components mean none), `rotation_type` (0 none, 1 linear, 2 quadratic, 3 TBC, 4 XYZ, 5 constant) with
`rotation_first`/`rotation_count` (quaternion keys in `QKEY`; for XYZ the float key group of the X angle, Y and Z
following it), `translation_type`/`translation_first`/`translation_count` (`VKEY`), `scale_group` (a float key
group). `Vec3Key` (56 bytes): time, value, the quadratic in and out tangents, TBC. `QuatKey` (32 bytes): time, value
(w x y z), TBC. Simulated: linear translation keys, XYZ rotation with float keys of any simulated float type, scale
keys; quaternion rotation keys and non-linear translation keys are stored and reported by the importer, not
simulated (no data spud loads uses them).

### Sequence and blocks

Name, weight, cycle type (0 loop, 1 reverse, 2 clamp), frequency, key range (`start_time`..`stop_time`), the
accumulation root name, its controlled blocks and text keys. A block binds the sequence's `interpolator` to a
`controller` with a `priority`; the five ID-tag strings (node name, property type, controller type, controller ID,
interpolator ID) are those the game binds by. For the emitter controller the interpolator ID says which input the
block drives (`BirthRate` or `EmitterActive`).

### EffectMaterial

`owner_kind` 1 = a particle system (`owner` = system index), 2 = a mesh shape of the same NIF whose shader one of
the effect's controllers animates (`owner` = the submesh index in the `.osm`, `k_none` if the shape was not
converted). The fields are BSEffectShaderProperty's (flags, UV offset/scale, base colour and scale, falloff as
stored, soft falloff depth, lighting influence, env-map min LOD, source and greyscale texture as normalised cache
paths) plus the NiAlphaProperty (decoded blend/test fields and the raw flags). `flags` uses `osm::MaterialFlag`.

## Tools

- `skytool fx convert <nif> [out.ose]` — one file; without `out.ose` the `.osm`, `.ose` and their textures go into
  the cache. `skytool import` writes `.ose` files for every converted NIF with particle systems and always converts
  the interface models no worldspace references (the Mist Menu fog).
- `skytool fx dump|sim|check|scan` — inspect, simulate at a fixed step, check determinism and steady-state counts,
  and convert + simulate every particle NIF in the data. ` runs the checks under the gate.

## Not covered (stored as warnings at import, not simulated)

Volume emitters (box, sphere, cylinder), colliders, bomb, wind, inherit-velocity, recycle-bound, Havok-update and
strip-update modifiers, the game, BSPSysMultiTargetEmitterCtlr, the NiPSys*Ctlr float controllers,
visibility controllers, Skyrim LE (BS 83) particle blocks, quaternion rotation keys and quadratic/TBC translation
keys. See "Still open" in the design.
