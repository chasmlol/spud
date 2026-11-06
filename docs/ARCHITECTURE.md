# Architecture

```
            user's Skyrim install (+ mods)                      spud runtime   
 ┌──────────────────────────────────────────────┐     ┌───────────────────────────────┐
 │ ESM/ESP/ESL  BSA  NIF  DDS  HKX  PEX  ...     │     │ engine (app, world streaming) │
 └───────────────┬──────────────────────────────┘     │ render (Vulkan)               │
                 │ read only by                       │ formats (native read)         │
        ┌────────▼─────────┐   writes native   ┌──────┴────────┐                      │
        │ importer          ├──────────────────►│ import cache  ├─────────────────────►│
        │ (legacy readers + │   assets locally  │ (spud fmt)    │   loads only native  │
        │  converters)      │                   └───────────────┘                      │
        └───────────────────┘                                     └───────────────────┘
```

## Layers

| Layer | Directory | Knows about Skyrim formats? |
|---|---|---|
| core | `src/core` | No. Logging, binary IO, paths, hashing, math. |
| formats | `src/formats` | No. Native spud formats: read/write. Shared by importer and engine. |
| importer | `src/importer` | **Yes — the only layer that does.** `legacy/` readers (BSA, ESM, NIF), `convert/` producers of native assets. |
| anim | `src/anim` | No. The behaviour-graph runtime and clip sampling, built from `; no rendering. |
| fx | `src/fx` | No. Particle effects simulated from `.ose` with the game's behaviour; no rendering, no Vulkan. |
| render | `src/render` | No. Vulkan renderer: device, bindless textures, meshes, terrain. |
| physics | `src/physics` | No. Jolt: static collision streamed per cell, the character controller. |
| ui | `src/ui` | No. Game UI: RmlUi documents (`data/ui`) on its own small Vulkan backend, composited by one renderer hook ([UI.md](UI.md)). |
| audio | `src/audio` | No. XAudio2 mixing on its own thread, Skyrim's sound, music and ambience rules (docs/AUDIO.md). |
| engine | `src/engine` | No. Application, world streaming, cameras, the player. |
| game | `src/game` | Conditions, and quests, the story manager and scenes over the forms database and the VM. |
| scripts | `src/script` | The Papyrus VM and natives over `.oss`. |
| ruleset (future) | `src/rules/tes5` | No formats — Skyrim's *rules* (actor values, conditions, Papyrus natives). Swappable for new games. |

Apps:

- `apps/skytool` — CLI: inspect legacy data (`bsa`, `esm`, `nif`), run the import (`import`) and convert or simulate particle effects (`fx`).
- `apps/spud` — the engine executable: main menu, settings and console, then a noclip world viewer.

## Import cache

The importer writes a self-contained directory (default `%LOCALAPPDATA%\spud\cache\skyrim`):

```
cache/
  meshes/...            .osm meshes, mirroring the source path (meshes\foo\bar.nif -> meshes/foo/bar.osm), and
                        .ose particle effects beside them for NIFs with particle systems
  meshes/.../*.osp      collision next to each mesh that has any
  textures/...          .dds textures, extracted as-is (generic format); the images the books name through
                        img:// (PNGs in the data) decoded into RGBA8.dds
  worlds/<edid>.osw     one compiled file per worldspace
  interiors/<edid>.osw  one per interior cell
  rules/game.osr        game settings, movement types, player dimensions, default key bindings
  forms.osf             every form of the load order (records, resolved text)
  skinned/meshes/.. what actors are drawn with:.osm with skin data,.osk skeletons
  animations/meshes/... behaviour projects, characters and graphs (.osb), animation skeletons (.osk), ragdolls (.ragdoll.osb), clips (.osn)
  animations/motion/... clip root motion per project (.osb)
  .import/              incremental state: compiled plugin units, asset keys, file hashes
  audio/sound.osd       every audio record of the load order, compiled (docs/formats/osd.md)
  sound/... music/...   sound files: WAV as shipped, xWMA as Ogg Vorbis, lip data as .osv (docs/formats/audio.md)
  import.log
```

Imports are deterministic and incremental: each plugin's records and each converted asset are keyed by the content hashes of what they came from, so a re-import after a mod or load-order change redoes only what changed and gives the same bytes as a full import. Mod Organizer 2 profiles import as the game sees them through MO2 (`skytool --mo2`).

## Native formats

All binary spud formats share the chunked **SPUD container** (`docs/formats/container.md`): a small header plus FourCC-tagged chunks, so readers skip what they don't understand and formats can grow without breaking.

| Format | Ext | Purpose | Spec |
|---|---|---|---|
| Mesh | `.osm` | Render meshes: submeshes, vertices, indices, materials; bones and weights for skinned models | `docs/formats/osm.md` |
| Skeleton | `.osk` | A skeleton's nodes at their rest pose | `docs/formats/osk.md` |
| Behaviour | `.osb` | A behaviour project, character or graph as an object graph; clip motion | `docs/formats/osb.md` |
| Face morphs | `.ofm` | One head part's morphs, from a `.tri` | `docs/formats/ofm.md` |
| Door animations | `.oda` | Each animated DOOR's Open and Close as rigid poses, with its sounds | `docs/formats/oda.md` |
| Clip | `.osn` | One animation clip as spline blocks, with annotations | `docs/formats/osn.md` |
| World | `.osw` | Compiled worldspace or interior: cell grid, terrain, placed references, texture/mesh tables, load doors, rooms and portals | `docs/formats/osw.md` |
| Physics shapes | `.osp` | Static collision of one mesh: primitives, hulls, triangle meshes, Skyrim layer and material | `docs/formats/osp.md` |
| Rules | `.osr` | Game settings and records that drive gameplay (movement for now), default key bindings | `docs/formats/osr.md` |
| Forms | `.osf` | Every form of the load order after overrides, fields in canonical form, with the record schema | `docs/formats/forms.md` |
| Particle effect | `.ose` | Particle systems, modifiers, emitter meshes, controllers, keys, sequences, effect materials | `docs/formats/ose.md` |
| Atmosphere | `.osa` | Climates and weathers exterior lighting is computed from | `docs/formats/osa.md` |
| UI movie | `.osu` | Compiled interface movies, string tables, font maps and controls | `docs/formats/osu.md` |
| Far view | `.osh` | A worldspace's generated far view: macro land maps, tree impostors and placements, region proxies | `docs/formats/osh.md` |
| Loading screens | `.osl` | The load order's loading screens with their conditions and the state those read | `docs/formats/osl.md` |
| Sound database | `.osd` | Audio records: categories, output models, sounds, music, reverb, ambience, footsteps, impacts, emitters | `docs/formats/osd.md` |
| Lip sync | `.osv` | One voice line's lip-sync track | `docs/formats/osv.md` |

These are *compiled runtime* formats. A human-editable authoring format for plugins (text, diffable, load order + override model) comes with the content milestone; it will compile to these.

Formats are defined at the milestone that first needs them, once the importer has shown what has to fit — not designed ahead.

## World model: spaces and portals

There are no load screens. The world is a set of **spaces**, each with its own coordinate frame, connected by **portals**:

- **Space.** A worldspace (Tamriel, Solstheim, Blackreach, ...) or an interior. Each has its own streaming grid, terrain (exteriors), instances and lighting rules (interiors use lighting templates, not the sun). Several spaces are loaded at once.
- **Cities are merged at import.** Skyrim's cities are child worldspaces (WhiterunWorld, SolitudeWorld, ...) that already share Tamriel's coordinates. The importer merges them into Tamriel and resolves the overlap from the records: Tamriel's stand-ins (`...LOD` statics) go, and so do its copies of what the city holds. City gates become ordinary doors.
- **Interiors connect through portals.** Interiors are bigger on the inside than the buildings they sit in, so they can't simply be placed in world space. Each pair of load doors (REFR `XTEL`) defines a portal with a rigid transform between the two spaces. An interior whose door is near the camera is streamed in early. It is rendered through the doorway (destination view = portal transform x camera, clipped to the door opening), and crossing the doorway moves the camera and the player into that space with no hitch. Portals chain: a dungeon of several interior cells is just more portals. has the details.
- **Logical identity is kept.** A merged city keeps a region tag for its original worldspace, and each interior stays its own cell. Conditions and scripts that ask "is the player in WhiterunWorld / this cell" keep working when gameplay arrives.

Performance: interiors are small (hundreds to a few thousand references), so preloading every interior within 40 m of the camera is cheap. A portal costs one extra scene pass, scissored to the door's screen rectangle and culled to its frustum, without recursion: one door's view at a time.

Deferred until gameplay: dynamic physics bodies and NPCs crossing portals, navmesh links through doors, sound propagation through doorways, opening animations for doors that were only ever load doors.

## LOD

Skyrim's LOD is replaced, not reproduced. The importer generates LOD from the full-detail data, and the renderer is GPU-driven so the whole map can stay on screen. For scale: Tamriel is only about 7 x 5.5 km, small by modern open-world standards.

**Selection is by screen-space error, not fixed distances.** Every LOD level knows its geometric error: how far, in metres, it strays from the full-detail shape. Each frame the renderer picks the coarsest level whose error projected onto the screen stays under the **pixel error threshold**. Resolution and FOV are therefore accounted for automatically: at 4K or when zoomed in, finer levels are kept longer. Because a swap only happens once the difference is sub-pixel, there are no fades (no dithered fades; see [RENDERING.md](RENDERING.md)). What is built is. If a swap is still visible, geometry morphs between levels by distance (continuous, never by time).

Four layers:

1. **Objects, near and mid range.** The importer generates a chain of 3–4 simplified levels per mesh with meshoptimizer (about 50%, 25% and 10% of the triangles). They share the original vertex buffer and store their geometric error. The same materials are used at every level.
2. **Far field: HLOD.** All large static objects in a region (4 x 4 cells, about 230 m) are merged and simplified, then merged again in a hierarchy (8 x 8 cells). Proxies keep the objects' own materials, on shared low-resolution copies of the same textures, so lighting matches across the swap and tiling textures stay sharp. Small clutter is left out; it is sub-pixel long before HLOD distance.
3. **Trees: octahedral impostors.** The full tree is rendered from many directions into an atlas with normals and depth, so impostors light correctly and read as 3D from any angle. They replace Skyrim's flat billboards.
4. **Terrain and grass.**
   - The full heightfield (about 3800 x 3000 samples at Skyrim's 1.8 m spacing, roughly 23 MB as 16-bit) stays resident. It is rendered with continuous LOD (CDLOD or clipmaps), geomorphing by camera distance. Far terrain uses a macro colour map baked at import from the real texture-layer blend.
   - Grass thins out with distance, with alpha-to-coverage and MSAA for a smooth fall-off into the terrain colour. No hard cutoff, no dithering.

**Rendering.** Instances live in GPU buffers. Compute passes do frustum and Hi-Z occlusion culling and per-instance LOD selection, feeding indirect multi-draw. Later, if needed, a cluster LOD hierarchy (Nanite-style, mesh shaders) can sit on top of the same data.

**Quality presets** use the same LOD data; only the thresholds change:

| Preset | Pixel error | Notes |
|---|---|---|
| Ultra | 0.5 px | Tracked in perf runs |
| **High** | **1.0 px** | **Default; the perf budget is measured here** |
| Medium | 2.0 px | |
| Low | 4.0 px | Handheld / min-spec |

Impostor switch distance and grass density scale with the preset. There is no hard object draw distance: the whole map is always drawn, coarser with distance. Streaming radius only decides what is resident at full detail.

**Skyrim's own LOD authoring is a hint, never a source.** Its LOD meshes and files (`.btr`/`.bto`/`.btt`, `lodsettings`) are not used. Per-reference flags such as "visible when distant" may still inform HLOD, for example keeping landmarks crisper.

## Conventions

- **Units:** metres. Skyrim units convert at import: `1 unit = 0.0142875 m` (70 units ≈ 1 m).
- **Axes:** right-handed, **Z up**, X east, Y north (same orientation as Skyrim).
- **Matrices:** column vectors, `world = R * local + T`.
- **Paths:** lowercase, forward slashes, relative to the cache root.
- **C++:** C++23, MSVC + CMake + Ninja + vcpkg (manifest mode). `snake_case` functions and variables, `PascalCase` types, `m_` prefix only where it helps.
