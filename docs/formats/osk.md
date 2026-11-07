# .osk — skeleton (kind `SKEL`, version 3)

An SPUD container (`container.md`): the node tree of one skeleton at its rest pose. A model skeleton is written by the
importer to `skinned/meshes/.../skeleton.osk`, an animation skeleton (the bones clips drive) to
`animations/meshes/.../skeleton.osk`. Skinned models name their bones by these nodes (`osm.md`, "Skinned models"), and
what hangs from a skeleton attaches to them. Reader and writer: `src/formats/osk.hpp`.

| Chunk | Contents |
|---|---|
| `BONE` | `BoneRecord[]`, parents before children: name (string offset), parent index (-1 for the root), rotation (3 x 3 floats, rows), translation (3 floats, metres), scale, flags (1: an animation bone that takes no translation from clips; 2: a tree's branch bone, which the wind moves) |
| `PROJ` | `u32`: a model skeleton's behaviour project (string offset), a cache path without extension, "" for none |
| `STRS` | String table |

`parent = rotation * (scale * local) + translation`. The root's own transform is identity: a reference's transform
takes the root's place, as for any model.

A tree model whose root is a `BSTreeNode` with branch bones has a skeleton beside it, `<model>.sway.osk`, with its
skinned copy `<model>.sway.osm` (`osm.md`): every node of the model, the branch bones (the `BSTreeNode`'s second bone list)
flagged 2.

## Versions

- 3: flag 2, a tree's branch bone. Version 2 files read as they are.
- 2: `PROJ`, the flags word.
- 1: first version.
