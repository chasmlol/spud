# .osh — far view of a worldspace (kind `FARV`, version 1)

Layout: [`src/formats/osh.hpp`](../../src/formats/osh.hpp). One file per worldspace with land, `far/<edid>.osh`, written by the last stage of the import from what it has just put in the cache. How it is made and drawn.

| Chunk | Contents |
|---|---|
| `INFO` | land cell bounds, texels per cell of the two macro maps, their paths |
| `MESH` | string offsets of the proxies' `.osm` paths |
| `IMPO` | `TreeImpostor[]`: colour and normal atlases, frames per side, the model's centre, radius, lateral reach |
| `TREE` | `TreeInstance[]`, 40 bytes, sorted by cell: position, scale, rotation, impostor, FormID |
| `TCEL` | `TreeCell[]` sorted as `CELL`: grid coords, first tree, count |
| `REGN` | `Region[]`, 64 bytes, level 1 then level 2: south-west cell, level, error, bounds, parent, proxies, origin |
| `PROX` | `Proxy[]`: index into `MESH`, directional material (`World::directional_materials`, or `k_none`) |
| `STRS` | string table |

## Semantics

- Macro maps: `far/<edid>/land.dds` (BC1) and `land_n.dds` (BC5), row 0 at the south edge of `cell_min`. Colour is the map the game's LOD landscape shader expects: the land's texture blend times its vertex colour, times the gain. Normal is the world normal's x and y, z positive.
- Impostors: `far/trees/<model>.dds` (BC3: albedo, coverage) and `<model>_n.dds` (BC1, half size: model-space normal, `n * 0.5 + 0.5`). Frames form a `frames x frames` grid over the hemi-octahedron, corners on the grid.
- Proxies: ordinary `.osm` files, `far/<edid>/r<level>_<x>_<y>_<n>.osm`, positions relative to `Region::centre`. Their materials name low-resolution copies of the source textures, `far/tex/<hash>.dds`.

## Versions

Readers accept only the current version.

- 1: first version.
