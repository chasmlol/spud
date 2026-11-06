# .osv — lip-sync track (kind `LIPS`, version 2)

Layout: [`src/formats/osv.hpp`](../../src/formats/osv.hpp). One per voice line, next to its audio in the cache
(`sound/voice/<plugin>/<voice type>/<line>.osv`), converted from the lip block of a Skyrim `.fuz` or a loose `.lip`
.

| Chunk | Contents |
|---|---|
| `HEAD` | frame count (u32), start frame (i32, the track's offset against the audio: negative means the lips lead), phoneme channel count (u32), modifier count (u32), frame rate (f32, always 30), compression (u32: 1 = LZ4 block), raw size of `DATA` decompressed (u32) |
| `DATA` | the frames, LZ4-compressed: frame-major `f32[frame_count][phonemes + modifiers]` |

## Semantics

- **Channels.** FaceGen heads have 16 phonemes (Aah, BigAah, BMP, ChJSh, DST, Eee, Eh, FV, I, K, N, Oh, OohQ, R, Th,
  W) and 17 modifiers. Creature races list
  their own channels in their RACE record (8 in vanilla) and have no modifiers.
- **Sampling.** 30 frames per second, no interpolation between frames: the game samples the frame under the current
  time.
- **Values** are the weights exactly as the file stores them (its zero-run coding is undone at import). The game
  multiplies the three head modifiers (14 HeadPitch, 15 HeadRoll, 16 HeadYaw) by `fDialogueHead*Exaggeration:Audio`
  (2.0) when it loads a line; that belongs to the playback side, so the stored values are unscaled. Most values are
  0, hence the compression.

## Versions

- 2: version 1's layout under a new name. The track was `.osl` until the audio workstream was merged with the UI
  workstream, whose loading screens keep `.osl`; caches made before the merge convert their lip tracks again.
- 1: first version.
