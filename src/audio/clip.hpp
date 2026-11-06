#pragma once

// Audio files of the import cache: 16-bit PCM WAV (kept from Skyrim as is) and Ogg Vorbis (Skyrim's xWMA
// and FUZ audio, re-encoded by the importer). See docs/formats/audio.md.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace spud::audio {

struct ClipInfo {
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    uint64_t frames = 0;
    // Loop region in frames, [begin, end). From a WAV "smpl" chunk's first loop when it has one, otherwise
    // the whole clip.
    uint32_t loop_begin = 0;
    uint32_t loop_end = 0;
    bool has_smpl_loop = false;
};

// A whole clip decoded to interleaved 16-bit PCM. Shared between sounds (read-only once loaded).
struct Clip {
    ClipInfo info;
    std::vector<int16_t> pcm;
};

// Streams a clip in pieces (Ogg Vorbis decodes as it goes; WAV reads from the file).
class ClipStream {
  public:
    virtual ~ClipStream() = default;
    virtual const ClipInfo& info() const = 0;
    // Decodes up to `frames` frames into `out` (interleaved); returns the number decoded (0 at the end).
    virtual size_t read(int16_t* out, size_t frames) = 0;
    virtual void seek(uint64_t frame) = 0;
};

// Throws Error on missing or malformed files.
std::shared_ptr<const Clip> load_clip(const std::filesystem::path& file);
std::unique_ptr<ClipStream> open_stream(const std::filesystem::path& file);
ClipInfo probe_clip(const std::filesystem::path& file);

} // namespace spud::audio
