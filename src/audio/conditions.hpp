#pragma once

// Condition evaluation for the audio rules: the CTDA lists of music tracks and sound descriptors, run with
// the player as subject and target. Only the functions audio records use are known; the general evaluator
// belongs to the conditions workstream (docs/AUDIO.md lists what is assumed until then).

#include "formats/osd.hpp"

#include <cstdint>
#include <span>

namespace spud::audio {

struct ConditionContext {
    float game_hour = 12.0f;
    uint32_t weather = 0; // current WTHR FormID
    bool interior = false;
    bool raining = false;
    bool snowing = false;
    bool in_combat = false;
};

// The game semantics: consecutive OR-flagged entries form a group, groups are ANDed; an empty list
// passes.
bool evaluate(std::span<const osd::Condition> conditions, const ConditionContext& context);

} // namespace spud::audio
