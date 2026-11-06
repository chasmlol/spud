# .oda — node animations (kind `NANM`, version 2)

How the moving parts of a model move: rigid poses in mesh space, sampled at 60 a second. For now one file, `rules/doors.oda`, written by `skytool import` (or `--doors-only`) from the DOOR records and their models' Open and Close sequences. A cache without it still loads: doors then don't move.

| Chunk | Contents |
|---|---|
| `DOOR` | one record per animated DOOR: FormID, mesh (string offset), open and close sound (SNDR FormIDs, 0 none), first part and count, first still and count, first backing and count, flags: 1 the moving parts have a back, 2 hung (no sequences in the game's file: spud's swing, for load doors only) |
| `PART` | submeshes that move together: first and count in `SUBS`; Open's and Close's first pose and count in `POSE`; its box in mesh space, low and high corners in metres |
| `SUBS` | `u32[]`: submesh indices of the door's `.osm` |
| `POSE` | quaternion xyzw and translation in metres: mesh space to mesh space, the identity where the shape stands in the file |
| `STRS` | strings |

- A part's pose maps the mesh onto itself, so its world pose is the reference's transform times the pose. Each sequence runs from its first pose to its last, so a track's length is `(count - 1) / 60` seconds.
- Still submeshes stay where they are (a door's frame). Backing submeshes are what stands behind a shut door (`DoorBlack`, `BlackPlane`), hidden while a door with something beyond it is open.

## Versions

- 1: doors.
- 2: hung doors. Same layout: the last field of `DOOR` is flags, and version 1 files read as they did.
