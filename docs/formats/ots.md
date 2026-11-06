# .ots — texture swaps (kind `TSWP`, version 2)

What a base object's alternate textures put on the shapes of its model. One file, `rules/texture_swaps.ots`, written by `skytool import`.

| Chunk | Contents |
|---|---|
| `SWAP` | one record per model of a base object: FormID, which model (0 an object's, 1 to 4 an armor addon's male, female, male first person, female first person), first shape, shape count |
| `SHAP` | a shape: submesh of the model's `.osm`, name (string offset), eight texture paths (string offsets) in `osm::TextureSlot` order, an empty string keeping the model's |
| `STRS` | strings |

- The source is the record's `MODS`, or for an ARMO its world model's `MO2S` against `MOD2`: a count, then per entry the shape's name, a TXST and a 3D index. A shape is found by name, without case, so every shape of that name takes the set. The 3D index isn't used.
- An armor addon's shapes go by name alone: its submesh is ~0.
- Statics go by the submesh, through a material variant of the reference (`MeshManager::directional_variant`). A skinned object's model has its own order, so it goes by the name.

## Versions

- 1: first version.
- 2: armor addons (`MO2S` to `MO5S`). `SWAP` gains the model. Version 1 files read as none.
