# Sound files in the cache

`skytool import` writes the game's sound and music files next to the sound database (`audio/sound.osd`,
[osd.md](osd.md)), under the same folders as in the game's archives, lower-cased: `<cache>/sound/...` and
`<cache>/music/...`. They are standard formats with no spud header; the database and the music tracks refer to
them by these cache paths. Code: `src/importer/convert/audio_codec.cpp` (import), `src/audio/clip.cpp` (runtime).

| Source in the game | In the cache | How |
|---|---|---|
| `.wav` | `.wav`, byte for byte | 16-bit PCM WAV, kept as it is, including its `smpl` chunk |
| `.xwm` (xWMA) | `.ogg` | decoded with Media Foundation's WMA decoder (`CLSID_CWMADecMediaObject`, WMAudioV8 input built from the file's `fmt` and the 0x1F encoder options), trimmed to the `dpds` table's total, re-encoded with libvorbis |
| `.fuz` (voice lines) | `.ogg` and `.osv` | the container's xWMA part as above; its lip block becomes the `.osv` lip-sync track ([osv.md](osv.md)) |
| `.lip` (loose) | `.osv` | as the lip block of a `.fuz` |

**Vorbis settings.** Same sample rate and channel count as the source. Quality: at least 0.4, raised until the
per-channel nominal bit rate is at least twice the source's (`vorbis_quality_for`). Ogg stream serial `0x4F534B59`
("SPUD") for every file, so a re-import is byte-identical. The runtime decodes with stb_vorbis: whole into memory, or
streamed when the decoded audio would exceed 4 MiB (music, long ambience).

**Checks.** The xWMA decode was compared with FFmpeg's decoder on sample files: same length, samples within 1 LSB.

**Updates.** A file already in the cache is not converted again unless `skytool import --force`; the records in
`sound.osd` are always rewritten. There is no version number: the files are WAV and Ogg Vorbis, and how they are made
is described here; changing it means re-importing with `--force`.
