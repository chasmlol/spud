#pragma once

// .osn: spud animation clip. Spec: docs/formats/osn.md
//
// One clip as B-spline blocks over its frames, kept in the compressed form the source has (
// "Spline compression"), so sampling gives the source's values at any time.

#include "formats/container.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace spud::osn {

inline constexpr container::FourCC k_kind = container::fourcc("CLIP");
inline constexpr uint32_t k_version = 1;     // a clip of spline blocks
inline constexpr uint32_t k_version_raw = 2; // one with `raw`

struct Annotation {
    float time = 0.0f; // seconds
    std::string text;
};

struct Clip {
    float duration = 0.0f; // seconds
    uint32_t transform_tracks = 0;
    uint32_t float_tracks = 0;
    uint32_t frames = 0;
    uint32_t blocks = 0;
    uint32_t max_frames_per_block = 0;
    uint32_t mask_size = 0; // maskAndQuantizationSize: bytes before a block's track data
    float block_duration = 0.0f;
    float block_inverse_duration = 0.0f;
    float frame_duration = 0.0f;
    bool additive = false;
    std::vector<uint32_t> block_offsets;
    std::vector<uint32_t> float_block_offsets;
    std::vector<uint8_t> data;
    std::vector<int16_t> track_bones;  // which bone each transform track drives; empty: track i drives bone i
    std::vector<int16_t> float_slots;  // the same for float tracks
    std::vector<Annotation> annotations; // by time
    std::vector<std::string> track_names; // the annotation tracks' names, one per transform track
    // Uncompressed, in place of the blocks: every frame's tracks, 12 floats each (translation, rotation, scale, as hkQsTransform).
    std::vector<float> raw;
};

std::vector<std::byte> write(const Clip& clip);
Clip read(std::span<const std::byte> file);

} // namespace spud::osn
