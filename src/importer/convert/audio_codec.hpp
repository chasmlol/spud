#pragma once

// Audio file conversion for the import: Skyrim's xWMA (WMA v2 in a RIFF "XWMA" container) and FUZ voice files
// in, generic formats out. WAV stays as it is; xWMA is decoded and re-encoded as Ogg Vorbis, which the
// runtime decodes. See docs/formats/audio.md.

#include "formats/osv.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spud::importer {

struct PcmClip {
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    std::vector<int16_t> samples; // interleaved
    size_t frames() const { return channels ? samples.size() / channels : 0; }
};

// The parts of a .fuz: lip-sync data and the audio file it wraps (xWMA, sometimes WAV).
struct FuzParts {
    std::span<const std::byte> lip;
    std::span<const std::byte> audio;
};
FuzParts split_fuz(std::span<const std::byte> fuz); // throws ParseError

bool is_xwma(std::span<const std::byte> file);

// Decodes a Skyrim lip block (the lip part of a .fuz, or a .lip file): docs/formats/osv.md describes the
// source layout and the native track. Throws ParseError on malformed data.
osv::LipTrack decode_lip(std::span<const std::byte> lip);

// Decodes an xWMA file to 16-bit PCM, trimmed to the length its "dpds" table declares. Throws Error.
// Thread-safe; each call uses its own decoder instance.
PcmClip decode_xwma(std::span<const std::byte> file);

// Encodes PCM as Ogg Vorbis (VBR at `quality`, -0.1..1.0), deterministic for the same input.
std::vector<std::byte> encode_vorbis(const PcmClip& clip, float quality);

// Vorbis quality chosen so the re-encode carries at least twice the source's bit rate: the generation
// loss of the transcode then stays far below what the source codec already discarded.
float vorbis_quality_for(uint32_t channels, uint32_t source_bytes_per_second);

} // namespace spud::importer
