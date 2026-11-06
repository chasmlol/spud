# .osw — compiled worldspace (kind `WRLD`, version 12)

Layout: [`src/formats/osw.hpp`](../../src/formats/osw.hpp). One file per space: a worldspace, `worlds/<edid>.osw`, or an interior cell, `interiors/<edid>.osw` (lowercase EditorID; `cell_<formid>` for a cell without one).

| Chunk | Contents |
|---|---|
| `WINF` | EditorID, name, parent world, FormID, flags, cell-grid bounds, cell size, default land/water heights, default land texture |
| `MESH` | mesh table: string offsets of `meshes/...osm` paths |
| `LTEX` | land textures: FormID, diffuse, normal, specular exponent (LTEX `SNAM`, default 30), flags (`land_texture_snow`: LTEX `INAM` non-zero) |
| `GRAS` | `Grass[]`: the GRAS forms its land textures grow (load-order winners): FormID, mesh, DATA's fields |
| `LGRS` | `u32[]`: each land texture's GNAM list in file order, as indices into `GRAS` (`LandTexture::first_grass`, `grass_count`) |
| `CELL` | `Cell[]` sorted by (y, x): grid coords, FormID, flags, water height, land index, ref range |
| `LAND` | `Land[]`: 33x33 heights (metres, absolute), normals, vertex colours, four quadrant headers |
| `LLYR` | `LandLayer[]`: texture slots 1..5 of a quadrant: land texture + the slot's 17x17 weight bytes |
| `REFS` | `Ref[]` grouped by cell: FormIDs, mesh index, flags, position, scale, rotation quaternion, source euler, directional material, tree and emittance indices |
| `EMIT` | `Emittance[]`: emittance sources of references (a light's colour, or a region's weather) |
| `LGHT` | `Light[]` sorted by cell, then FormID: placed point lights (position, radius, colour, dimmer, flags, animation) |
| `DMAT` | `DirectionalMaterial[]`: per base STAT with a directional material or the snow flag: MATO values, max angle, flags |
| `TREE` | `Tree[]`: per TREE base whose model's root is a leaf-animation node: CNAM leaf amplitude and frequency |
| `DPAS` | `DirectionalPass[]`: a multi-pass MATO's DNAM lighting property (shader type, flags, UV transform, emissive, alpha, specular, glossiness, sparkle, diffuse and back-light textures) |
| `CURC` | water currents: `CellCurrent[]` sorted by (y, x): grid coords, first quad, quad count |
| `CURQ` | water current quads: 16 bytes each, the game's element format |
| `PRIM` | collision primitives: `Primitive[]` sorted by (y, x), 68 bytes: grid coords, the reference's FormID, type (1 box, 2 sphere, 3 plane), Skyrim collision layer, ref flags, position, rotation quaternion, half extents |
| `RGNS` | `Region[]`: the child worldspaces merged into this one (FormID, EditorID) |
| `CRGN` | `u16[]`, parallel to `CELL`: 0 this world, else 1 + index into `RGNS`; empty when nothing is merged |
| `WSRF`, `WVTX` | placed water: each water activator reference's shapes with a water shader as a triangle list in world metres, with its WATR FormID and bound |
| `FLWC`, `FLWT` | the cells with a flow map (`FlowCell`, sorted by y, x) and their 64 x 64 RGBA8 texels, from `textures/water/skyrim.esm/flow.X.Y.dds`; Tamriel only |
| `CWAT` | `u32[]`, parallel to `CELL`: the cell's WATR FormID (XCWT, else the world's), 0 without water; the form's data is in `.osa` |
| `ILGT` | `CellLighting`: an interior's resolved lighting (zero for worldspaces) |
| `DOOR` | `Door[]` sorted by FormID: load doors, with the far door's frame |
| `ROOM` | `Room[]` sorted by FormID: room boxes, their links, lighting template and image space |
| `RLNK` | `u32[]`: rooms named by `XLRM`, as indices into `ROOM` |
| `RLGT` | `CellLighting[]`: the templates of interior rooms that have one (`Room::lighting`) |
| `PORT` | `Portal[]` sorted by FormID: portal rectangles and their two rooms (`k_none` = outside) |
| `OCCL` | `Occluder[]` sorted by FormID: occlusion planes and boxes |
| `STRS` | string table |

## Semantics

- This is the **final state at game start**: the whole load order is applied (later plugins override earlier ones), deleted records are removed.
- Coordinates are metres, Z up, X east, Y north. A cell at grid `(x, y)` covers `[x, x+1) * cell_size` by `[y, y+1) * cell_size`.
- Terrain: vertex `(row, col)` of a cell sits at `(x * cell_size + col * cell_size / 32, y * cell_size + row * cell_size / 32)`. Row 0 is the south edge.
- Quadrant `q` (0 SW, 1 SE, 2 NW, 3 NE) covers rows/cols `[0..16]` or `[16..32]`. It is drawn as the game draws it: slot 0 is the base texture, slots 1..5 the layers (`layer_count` <= 5), each with the game's weight byte per vertex after its layer merging; the base's weight is `clamp(255 - sum of the layers')`. Heights are the game's (8 x the truncated running sum of the VHGT deltas).
- Persistent references (which Skyrim stores in a world-level cell) are placed in the grid cell containing their position and flagged `ref_persistent`.
- `ref_initially_disabled` marks references hidden at game start, directly or through their enable parent. Viewers hide them by default.
- `ref_fade_items`, `ref_fade_sky_cell`, `ref_fade_visible_when_distant` and `ref_fade_never` give the category of the reference's fade node, which scales its LOD fade metric; a reference with none of them fades as an object.
- References with no renderable mesh (markers, lights without models, triggers) are not exported as `Ref`s.
- Lights: every reference of a `LIGH` base that the game turns into a point light, with the values the engine computes when it creates the light: radius = DATA radius + `XRDS` (<= 0 becomes 0.1 units), colour = DATA colour / 255 (negated for the Negative flag; an `XEMI` light source multiplies it by that light's colour, a region source sets `light_region_emittance`), dimmer = `FNAM` + `XLIG` fade offset. Not exported: references with the "turn off fire" flag (0x80), bases flagged "off by default" (0x20), lights below their cell's water. `light_initially_disabled` follows the same rule as references. A light with a model sits at the reference position (the model's `AttachLight` offset is not applied). `phase` holds the animation's starting phases (constant, linear, quadratic attenuation: 0, 0, 16 / int(radius in units)^2).
- FormIDs are load-order resolved (`0xLLxxxxxx`, ESL `0xFEnnnxxx`) so they match the in-game console. They are a cross-reference for checks, not an engine identity.
- **Water currents** are the game's per-cell current list, built the way the game builds it: the cell's XWCN/XWCU zones, then each CellWaterCurrentMarker (STAT 00000061) placed in the cell sets the first uniform element's velocity to the marker's forward axis × its scale (or appends one), then the cell water's WATR NAM0/NAM1 fill a still-zero uniform element or make one when the cell has none. Elements are 3 quads (type 0 uniform, 1 circle) or 4 (2 capsule, 3 half-disc); quad 0 = linear velocity (w: 0 world vector, else polar), quad 1 = angular velocity (w: weight at the zone edge), quad 2 (w: the type as int32) and quad 3 = the shape. Velocities are Havok m/s, positions Havok units (metres). Only cells with water carry a list.
- **Collision primitives**: every CollisionMarker (STAT 00000021) reference with an XPRM primitive of type 1–3 is a solid shape of its own (invisible walls): half extents are XPRM's bounds in metres, not scaled by the reference (a sphere's radius in `half_extents[0]`, a plane's extents in x and z); `layer` is XTRI's layer number, else 3 (TRANSPARENT). The engine gives boxes Havok's 0.05 m convex radius.

- Directional materials: a STAT's `DNAM` (max angle, MATO, and from form version 44 a flags byte whose bit 0 marks the object as snow) and its `MATO` `DATA` (52 bytes; a shorter one keeps the class defaults 0.4, 0.4, 512, 512, (0, 0, -1), 1, (0, 0, 0), 0, 0). One `DirectionalMaterial` per base that has a MATO or the snow flag; `Ref::directional` indexes it (`k_none` otherwise). How the engine applies it, section 11.

- **Merged cities:** a world holds the child worldspaces that are cities and share its coordinates, with its stand-ins and duplicates removed. The child's own `.osw` is still written.
- **Interiors** (`world_interior`): one source cell, split over grid cells by reference position so it streams like a worldspace; every `Cell` carries the interior's FormID, none has land. `world_show_sky` and `world_use_sky_lighting` are CELL DATA 0x80 and 0x100. `CellLighting` is `XCLL` with the template fields its inherit flags name; a light fade of 0 stands for the INI settings; `sunlight_scale` is the image space's.
- **Doors:** every door reference with `XTEL`. Exported whether or not it has a mesh. `marker_position` is the destination door's own `XTEL` position when it leads back here (`door_has_marker`), which is what tells a door's front. `destination_space` names the file; a child world merged into a parent is named as the parent.
- `light_portal_strict` is LIGH flag 0x2000: the light reaches only rooms joined by portals to the ones holding it.
- **`Ref::room_hint`:** the room of the reference's `XMBR`, or `k_none`.
- `ref_occludable` marks architecture (STAT, SCOL, MSTT) and `ref_occludable_far` trees and flora: what occlusion planes may hide.

## Versions

Readers accept only the current version: earlier ones have other `Ref` and `LandTexture` layouts, so the import is
redone.

- 16: `WaterSurface` gains `plane` and `vertex_uv`, one surface per water shape; `WaterVertex` gains `depth`.
- 15: `GRAS` and `LGRS` chunks; `LTEX` records gain `first_grass` and `grass_count`.
- 14: `WSRF`, `WVTX` chunks: placed water.
- 13: `FLWC`, `FLWT` chunks: flow maps.
- 12: `CWAT` chunk: each cell's water form.
- 11: `world_no_sky`.
- 10: `Light` gains the shadow fields (`shadow_fov`, `shadow_falloff`, `shadow_near`, `shadow_bias`), 112 bytes.
- 9: spaces: `RGNS`, `CRGN`, `ILGT`, `DOOR`, `ROOM`, `RLNK`, `RLGT`, `PORT`, `OCCL` chunks, `Ref::room_hint`, interiors as files.
- 8: `CURC` and `CURQ` chunks (water currents) and `PRIM` chunk (collision primitives), from the movement
  workstream. Its branch called them version 2 of its own line; those files predate the merge and are re-imported.
- 7: `Ref::flags` bits 2-5, the fade node's category.
- 6: `EMIT` chunk and `Ref::emittance`: the reference's emittance source (`XEMI`), the colour its shapes with
  External_Emittance take.
- 5: `DPAS` chunk and `DirectionalMaterial::pass` (the multi-pass MATO's material).
- 4: `TREE` chunk and `Ref::tree` (leaf animation); `Ref` is 80 bytes.
- 3: `LGHT` chunk (placed point lights); `DMAT` chunk and `Ref::directional` (projected-UV snow, moss and ash).
- 2: land as the game draws it: `LTEX` flags, `LLYR` holds slot weight bytes (at most 5 layers per quadrant), heights truncated like the game's.
- 1: first version.
