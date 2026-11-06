# .osm — mesh (kind `MESH`, version 10)

Layout: [`src/formats/osm.hpp`](../../src/formats/osm.hpp).

| Chunk | Contents |
|---|---|
| `INFO` | bounds (min/max, metres), vertex/index/submesh/material counts, the root's bound sphere (centre, radius; metres) |
| `VERT` | `Vertex[]` — 36 bytes: position f32x3 and bitangent x f32, uv f32x2, normal unorm8x4 (w = bitangent y), tangent unorm8x4 (w = bitangent z), colour rgba8 |
| `INDX` | `u32[]` triangle list, indices relative to the submesh's `base_vertex` |
| `SUBM` | `Submesh[]` — 128 bytes: first index, index count, base vertex, vertex count, material, billboard mode, shape transform (3x4 floats), billboard transform (3x4 floats) |
| `LODS` | `SubmeshLod[]`: 3 per submesh, or none: first index, index count (0: no such level), error in metres. The levels' indices follow the submeshes' in `INDX` and use the same vertices |
| `MATL` | `MaterialParams` + 8 texture string offsets per material |
| `UVCT` | material controllers: material, variable (`osm::MaterialVariable`: u/v offset and scale, alpha, emissive multiple and colour, specular strength and colour, glossiness, environment-map scale, refraction strength; effect base colour, its scale and alpha, falloff angles and opacities; alpha-test reference), key type, NiTimeController flags, frequency, phase, start, stop, value, key range |
| `KEYS` | controller keys, 5 floats each: time, value, then forward/backward tangents or tension/bias/continuity |
| `STRS` | string table |

## Semantics

- Positions are in each submesh's **shape space, metres**; `Submesh::transform` (rows of `[R * s | t]`) maps them into mesh space. Rigid shapes keep the NIF's own vertex data unchanged, with the node transforms on the submesh, so shading sees the game's exact bytes. Skinned shapes are posed at import (the stored pose) with an identity transform and a re-packed frame.
- The tangent frame is the game's packing: each unorm8 component decodes as `2v - 1`; N = normal.xyz, T = tangent.xyz, B = (bitangent_x, normal.w, tangent.w). The normal map's x follows B and its y follows T. Shapes without a stored frame (NiTriShapeData or none) are packed by rounding to nearest.
- Within a submesh, vertices are in fetch order and triangles in vertex-cache order. Triangles keep the source's order where it shows: blended and decal materials, and those that don't both test and write depth.
- Nodes the original engine never renders (editor markers, collision) are dropped.
- Materials keep Skyrim's lighting-model data verbatim (`skyrim_shader_type`, `skyrim_flags1/2`, specular, glossiness, rim/soft lighting, env map scale, type-specific parameters). The renderer uses them to reproduce Skyrim's shading for imported content. `flags` holds spud's own decoded flags (alpha test/blend, two-sided, decal, vertex colours, ...).
- `type_params` by `skyrim_shader_type`:
  - 1 environment map: `[0]` env map scale (also in `env_map_scale`)
  - 5 skin tint: `[0..2]` tint colour
  - 6 hair tint: `[0..2]` tint colour
  - 7 parallax occlusion: `[0]` max passes, `[1]` scale
  - 11 multilayer parallax: `[0]` inner thickness, `[1]` refraction scale, `[2..3]` inner UV scale, `[4]` env map strength
  - 14 sparkle snow: `[0..3]` sparkle parameters
  - 16 eye: `[0]` cubemap scale, `[1..3]` left centre, `[4..6]` right centre
- Texture paths are normalised cache paths (`textures/...dds`).

## Skinned models

A model drawn on a skeleton (`skinned/meshes/...`) has more chunks; a static model has none of them.

| Chunk | Contents |
|---|---|
| `SKVT` | `SkinVertex[]`, parallel to `VERT`: four half-float weights, four bone bytes indexing the shape's bones |
| `SKBN` | `SkinBoneRecord[]`: the skeleton node's name (string offset) and the skin-to-bone transform, rows of `[R * scale \| t]` (metres) |
| `SKSH` | `SkinShape[]`, parallel to `SUBM`: the shape's bones and dismember partitions |
| `SKPT` | `u16[]`: the partitions' body parts (BSDismemberSkinInstance) |
| `SKPI` | `u32[]`, parallel to `SKPT`: the indices of each partition, in order from the submesh's first index (all 0 when the shape's triangles aren't kept by partition) |
| `SKMO` | `Vertex[]`, parallel to `VERT` or empty: the `_0` end of a weight-morphed model, whose `VERT` is the `_1` end |
| `SKAT` | `u32`: the node the model's `Prn` extra data names (string offset) |

Vertices stay in skin space (the submesh transform is identity), with the game's bytes. A rigid shape in such a model
is skinned wholly to one bone named `""`: the node the actor holds the model at, its `Prn` extra data's while a weapon is
sheathed. A weapon's scabbard (below its `Scb` node) names the `Prn` node itself, so it stays there.
Skinned models have no `LODS` entries. The engine poses them.

A destructible's model at a `BSDamageStage` stage is an ordinary `.osm` beside the intact one, `<model>.stage<N>.osm`, with the nodes that stage shows; its collision is `<model>.stage<N>.osp`.

## Versions

- 11: `SKNM`: each skinned shape's name (string offsets, parallel to `SKSH`), which ties a FaceGeom shape to its head part. A head part's shapes keep the NIF's vertex order, for its morphs (docs/formats/ofm.md). Versions 9 and 10 read as they are.
- 10: skinned models (the `SK..` chunks). Version 9 files read as they are.
- 9: `LODS` chunk.
- 8: `INFO` ends with the root node's bound sphere (`fade_bound`: centre in mesh space and radius, metres;
  radius 0 when the model has none) as the game merges it from the NIF's node and shape bounds
. LOD fades and light reach measure from it.
- 7: `MaterialParams` keeps an effect's lighting influence byte in what was `reserved0` (255 when the NIF's is 0,
  as the game loads it).
- 6: the packed tangent frame is the game's bytes for every rigid BSTriShape, as version 2 meant: versions 2-5 rebuilt
  it from the UVs for every shape that stores its own vertices. A BSDynamicTriShape's
  bitangent x is the w of its float4 positions.
- 5: every shader-property controller, not only UV ones: lighting and effect float and colour controllers and the
  alpha property's test-reference controller; a colour controller is stored as one
  controller per component.
- 4: billboards: a shape under an NiBillboardNode keeps its transform below the node, and the node's mesh-space frame and mode go on the submesh; UV controllers without keys, inactive or driven by blend interpolators are dropped; TBC keys are stored as Hermite keys.
- 3: UV controllers; `Submesh` gains the billboard mode and transform (128 bytes).
- 2: the game's packed tangent frame and per-submesh shape transforms. Importers re-convert meshes written by older versions.
- 1: baked node transforms, snorm8 frame recomputed from UVs.
