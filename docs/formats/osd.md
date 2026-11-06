# .osd — sound database (kind `SNDB`, version 1)

Layout: [`src/formats/osd.hpp`](../../src/formats/osd.hpp). One file per import: `<cache>/audio/sound.osd`, written
by `skytool import` (`--audio-only` imports nothing else). It holds every audio record of the load order, compiled for
the runtime (src/audio), plus the audio-relevant parts of cells, worldspaces and locations and every placed
reference that emits sound. The sound files themselves are separate cache files (see "Sound files" below).

Records keep their load-order FormIDs (the winning override). Cross-references between records are **indices** into
the arrays below, `0xFFFFFFFF` (`k_none`) when absent; references to forms the database doesn't hold as a table
(MATT on an impact pair, REGN on an acoustic space, LCTN parents, WRLD parents) stay FormIDs. Strings are offsets
into `STRS` (NUL-terminated). Distances are **metres** and positions world space (Z up), converted from Skyrim units
(1 unit = 0.0142875 m); everything else keeps the record's units unless a field says otherwise.

| Chunk | Contents |
|---|---|
| `CATG` | `Category[]` (SNCT): parent index, flags (mute when submerged, on menu), VNAM/65535, UNAM/65535 (1.0 when absent) |
| `OMDL` | `OutputModel[]` (SOPM): attenuates/rumble flags, HRTF or defined speakers, reverb send 0..1, min/max distance (m), the five-point attenuation curve (percent), the ONAM speaker levels (rows mono, stereo L, stereo R; columns FL FR C LFE RL RR BL BR) |
| `COND` | `Condition[]` (CTDA, 32 bytes): operator byte, comparison (float bits, or a GLOB FormID), function, parameters with form parameters resolved to load-order FormIDs, run-on, reference |
| `DESC` | `Descriptor[]` (SNDR): category, output model, "alternate sound for", file list, flags (the game's loop/envelope bits plus the raw LNAM bytes, rumble in the top byte), frequency shift/variance (%), priority, dB variance, static attenuation (hundredths of a dB), conditions |
| `FILS` | `u32[]` string offsets: each descriptor's files, as cache paths (`sound/fx/....wav` or `.ogg`) |
| `SOUN` | `SoundMarker[]` (SOUN): the SNDR it plays |
| `REVB` | `Reverb[]` (REVB DATA as stored: decay ms, HF reference Hz, room filter, room HF filter, reflections, reverb amp in the record's units; decay HF ratio /100, reflect delay x1.2 ms, reverb delay ms, diffusion %, density %) |
| `ASPC` | `AcousticSpace[]` (ASPC): ambient loop descriptor, region (REGN FormID), reverb index, OBND (m) |
| `MUSC` | `MusicType[]`: flags (FNAM), priority, ducking (hundredths of a dB), fade duration (s), track list |
| `MUST` | `MusicTrack[]`: palette / single / silent, duration FLTV and fade-out DNAM (s), file and finale (cache paths), LNAM loop (s, count), cue points, conditions, palette sub-tracks |
| `TRKL` | `u32[]` music track indices (type track lists and palette sub-track lists; `k_none` keeps a palette's empty entries, which split its layers) |
| `CUES` | `f32[]` cue points (s), sorted ascending |
| `FSTP` | `Footstep[]`: impact data set, tag string |
| `FSTS` | `FootstepSet[]`: per gait (walk, run, sprint, sneak, swim) a range in `FSTL` |
| `FSTL` | `u32[]` footstep indices |
| `IPCT` | `Impact[]`: effect duration, orientation, angle threshold, placement radius (m), sound level, flags, result, sound 1 and 2 (descriptor indices) |
| `IPDS` | `ImpactSet[]`: a range in `IPPR` |
| `IPPR` | `ImpactPair[]`: MATT FormID, impact index |
| `MATT` | `Material[]`: parent, flags, Havok impact set, MNAM name |
| `REGN` | `Region[]`: worldspace, music (RDMO), sound priority/override (RDAT), deleted flag, sounds (`RSND`), areas (`RARA`) |
| `RSND` | `RegionSound[]` (RDSA): descriptor, flags (pleasant/cloudy/rainy/snowy), chance |
| `RARA` | `RegionArea[]`: a polygon, a range in `RPTS` |
| `RPTS` | `f32[]` polygon points, x/y pairs in metres |
| `WATR` | `Water[]`: SNAM sound descriptor, FNAM flags |
| `DOBJ` | `DefaultObject[]`: signature and FormID of every default object, merged over all plugins in load order (later plugins override per signature; empty entries skipped) |
| `WTHR` | `Weather[]`: DATA as stored (20 bytes) and the SNAM sounds (`WSND`) |
| `WSND` | `WeatherSound[]`: descriptor, type (default, precipitation, wind, thunder) |
| `WRLD` | `World[]`: parent, music (ZNAM), location, water |
| `LCTN` | `Location[]`: parent, music (NAM1) |
| `CELL` | `Cell[]`: world, grid, DATA flags as stored (interior 0x1, has water 0x2, show sky 0x80), acoustic space (XCAS), music (XCMO), location (encounter zone's location, else XLCN), regions (`CREG`), emitters (`EMIT`), water (XCWT) and water height (m, with Skyrim's sentinel rules applied) |
| `CREG` | `u32[]` region indices per cell (XCLR) |
| `EMIT` | `Emitter[]`: every placed reference that makes sound: sound markers (SOUN), acoustic spaces (ASPC, with XPRM box/sphere bounds), looping ACTI/MSTT/TACT sounds, LIGH sounds, DOOR loop sounds. Kind, initially-disabled flag, descriptor, acoustic space, position (m), rotation (quaternion xyzw), scale, XPRM bounds and type, and for line sound markers (XPRM type 4) the segment's start and end (m) |
| `SETT` | `Setting[]`: INI and game settings the audio rules read ("fReverbTransitionTime:Audio" = value): the game's defaults, overridden by Skyrim_Default.ini |
| `STRS` | string table |

## Semantics

- **Load order.** The importer reads the user's plugins in load order; the last override wins, except `DOBJ`, which
  merges every version (a Creation Club plugin overrides only part of the list).
- **Exterior cells and persistent references.** References in a worldspace's persistent cell are assigned to the
  exterior cell containing them, so emitters can be streamed by grid like everything else.
- **What is not resolved at import.** Conditions, region polygons, weather classification and the music rules are
  evaluated at runtime; the database only stores the data they need.
- **Sound files.** Descriptor and music paths are cache-relative and lower-case. `.wav` files are copied from the
  archives unchanged; `.xwm` and the audio of `.fuz` files are decoded (Media Foundation's WMA decoder, checked
  against FFmpeg to the last bit) and re-encoded as Ogg Vorbis (`.ogg`). The lip
  block of a `.fuz` (and loose `.lip` files) become `.osv` (docs/formats/osv.md) next to the audio. A path in the
  database names the file the runtime opens; the importer resolves the game's extension fallback (.wav, then .fuz,
  then .xwm) when it writes it.

## Versions

- 1: first version.
