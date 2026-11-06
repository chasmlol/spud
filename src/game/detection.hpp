#pragma once

#include <array>

// Detection: what one actor makes of another., "Detection".

#include "game/settings.hpp"

namespace spud::game {

// Actor high data: one event, overwritten. `left`: the seconds it counts (*inferred*: the action sound's timer).
struct DetectionEvent {
    int level = 0;
    std::array<float, 3> position{};
    float left = 0;
};

struct DetectionInput {
    float distance = 0;      // units, 3D
    float angle = 0;         // degrees between the observer's heading and the target
    bool exterior = false;
    bool los = false;        // a ray reached the target
    float observer_skill = 0; // Sneak
    float target_skill = 0;
    bool sneaking = false, moving = false, running = false, invisible = false;
    float light = 0;          // L: after the interior's ambient and the clamp
    float noise_mult = 1;     // actor value 92
    float worn_weight = 0;
    int action_sound = 0;     // a swing or a shot within the alert time
    float size = 1;           // fSneakDetectionSize*
    bool observer_in_combat = false, observer_asleep = false, observer_busy = false;
    float blindness = 0;
};

// How far a test of sight reaches, for `angle` at the observer (step 5): the caller casts rays inside it only.
float detection_sight_range(const Settings& s, bool exterior, float angle);
// The level: negative unaware, above 0 detected.
// Stage B, the player as target: the game within the radius. `line`: nothing between.
bool event_heard(const Settings& s, int level, float distance, bool exterior, bool line, bool asleep);
int detection_level(const Settings& s, const DetectionInput& in);
// A player's Sneak counts as this on either side.
float player_detection_skill(const Settings& s, float sneak);

} // namespace spud::game
