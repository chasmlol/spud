#pragma once

// .osv — spud lip-sync track. Spec: docs/formats/osv.md
//
// One per voice line, converted from the lip block of a Skyrim .fuz (or a loose .lip). A track is a run of
// frames at 30 per second; each frame holds one weight per phoneme channel and, for FaceGen heads, 17
// modifier values. Frames are stored LZ4-compressed: most channels are 0 most of the time.

#include "formats/container.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace spud::osv {

inline constexpr container::FourCC k_kind = container::fourcc("LIPS");
inline constexpr uint32_t k_version = 2; // 2: version 1's layout, renamed from .osl (docs/formats/osv.md)
inline constexpr float k_frame_rate = 30.0f;

struct LipTrack {
    int32_t start_frame = 0;      // the track's offset against the audio, frames (negative: lip leads)
    uint32_t phoneme_count = 16;  // 16 FaceGen phonemes, or a creature race's channels (8 in vanilla)
    uint32_t modifier_count = 17; // 17 FaceGen modifiers, or 0
    std::vector<float> frames;    // frame-major: phonemes then modifiers
    uint32_t frame_count() const {
        const uint32_t row = phoneme_count + modifier_count;
        return row ? static_cast<uint32_t>(frames.size() / row) : 0;
    }
};

std::vector<std::byte> write(const LipTrack& track);
LipTrack read(std::span<const std::byte> file);

} // namespace spud::osv
